# Реверс-инжиниринг системы повреждений и деформации кузова

Документ фиксирует устройство системы деформации и разрушения кузова в **CarX Drift Racing Online 2**, причину прохождения урона при переворотах в режиме «Без повреждений» и реализованное решение в `Urushi`.

---

## 1. Архитектура повреждений и деформации

Обработка повреждений автомобиля состоит из двух слоёв:

### Слой 1: MonoBehaviour / IL2CPP (`GameAssembly.dll`)
- `BaseCollisionEffectBehaviour::Apply` — визуальные и физические эффекты столкновения.
- `CarPartsPhysicsController::DeleteDetachedParts` и `CarPartsPhysicsController::OnTuningPartDeformationStateChangedHandler` — отрыв и удаление съёмных/тюнинг-деталей (бампера, крылья и т.д.).
- `CollisionEventsHandler::CollisionEnter` и `CollisionEventsHandler::CollisionStay` — обработка коллизий для навесных элементов/деталей (метод `CollisionExit` отвечает только за уведомление слушателей выхода и сброс `m_collisionExitInProgress`, его блокировать нельзя).
- `PhysicsBehaviour::Detach` (`CarX.DeformationPhysics`) — отсоединение физических элементов.
- Статические настройки систем сна в DOTS:
  - `DeformationSkeletonSleepSystem` (`+0x00 enabled`, `+0x04 idleSteps`, `+0x08 farDistance`) — при `noDamage` выставляется `enabled=1, idleSteps=1, farDistance=0.0f`.
  - `PhysicsRootSleepSystem` (`+0x00 enabled`, `+0x04 idleSteps`) — при `noDamage` выставляется `enabled=1, idleSteps=1`.

### Слой 2: Unity DOTS / Burst (`CarX.DeformationPhysics.Systems`)
Две ECS-системы, управляющие самой деформацией скелета кузова и разрывом джоинтов:
1. `CarX.DeformationPhysics.Systems.ApplyForceToDeformationSkeletonSystem`
2. `CarX.DeformationPhysics.Systems.BreakPhysicsJointSystem`

На ветке `alpha` (`GameAssembly.dll ts=0x6AEDE413 size=0x0A7AE000`, `lib_burst_generated.dll size=0xDF6000`):
- `ApplyForceToDeformationSkeletonSystem` скомпилирована через **Unity Burst** (`flags = 0x70027`) и выполняется внутри `lib_burst_generated.dll`.
- `BreakPhysicsJointSystem` на момент проверки не скомпилирована в Burst (`flags = 0x23`), её `SystemBaseRegistry` thunk указывает внутрь `GameAssembly.dll`.

---

## 2. Почему проходил урон при переворотах

1. Ранее `Urushi` ставил MinHook-перехваты только на IL2CPP-методы `ApplyForceToDeformationSkeletonSystem::__codegen__OnUpdate` и `BreakPhysicsJointSystem::__codegen__OnUpdate` / `OnUpdate` в `GameAssembly.dll`, а также усыплял `DeformationSkeletonSleepSystem`.
2. Однако в рантайме ECS-планировщик (`UnmanagedUpdate`) берёт указатель на `OnUpdate` не из IL2CPP `MethodInfo` напрямую, а из `Unity.Entities.SystemBaseRegistry::s_Data` (`SystemTypeInfo[metaIdx] + 0x08`), который для `ApplyForceToDeformationSkeletonSystem` ведёт в **`lib_burst_generated.dll`**, минуя IL2CPP-метод `__codegen__OnUpdate` в `GameAssembly.dll`.
3. При обычном касании часть повреждений отсекалась перехватами `CollisionEventsHandler` / `BaseCollisionEffectBehaviour` / `PhysicsBehaviour::Detach`, а при перевороте движение шасси удерживало скелет деформации активным (не давая `DeformationSkeletonSleepSystem` усыпить его), и импульсы контакта крыши/капота/переда с землёй напрямую заходили в Burst-версию `ApplyForceToDeformationSkeletonSystem`.
4. Поскольку `BreakPhysicsJointSystem` оставалась не-Burst, счётчик `Детали Блок` оставался равен 0, съёмные детали (при достаточном покрытии другими хуками) держались, но сам несъёмный скелет кузова (капот, передние крылья, силовая структура) сминался.

---

## 3. Динамическое разрешение Burst-систем без хардкода RVA

Реализация в [car_sim_optimizer_hooks.cpp](file:///C:/Program%20Files%20(x86)/Steam/steamapps/common/CarX%20Drift%20Racing%20Online%202/Urushi/core/car_sim_optimizer_hooks.cpp) (`TryInstallBurstDeformationHooks`, вызывается из `ResolveStaticsIfNeeded`) не использует фиксированных RVA и работает независимо от конкретной сборки (`release` / `alpha`):

1. **Поиск ID систем в `Unity.Entities.TypeManager.SharedSystemTypeNames`**:
   - Ищем `Unity.Entities.TypeManager` через `Il2CppHelpers::LookupTypeClass("TypeManager", "Unity.Entities")`.
   - По массиву вложенных типов (`+0xA0`, количество `uint16_t` по `+0x128`) находим класс `SharedSystemTypeNames` (проверка `+0x50 == TypeManager`).
   - Из `static_fields` (`+0xB8`): `+0x00` -> коробка `entries`, `+0x20` -> коробка `count`.
   - Элемент таблицы имеет шаг `0x18`: `+0x00 const char*`, `+0x08 uint32_t len` (важно: `len` в `SharedSystemTypeNames` включает завершающий `\0`, т.е. равен `strlen(name) + 1`).
   - Ищем индексы для:
     - `CarX.DeformationPhysics.Systems.ApplyForceToDeformationSkeletonSystem` (на `alpha` это `id=152`, `len=70`)
     - `CarX.DeformationPhysics.Systems.BreakPhysicsJointSystem` (на `alpha` это `id=153`, `len=56`)

2. **Поиск `metaIdx` в активном мире (`Unity.Entities.World`)**:
   - Читаем `World.DefaultGameObjectInjectionWorld` (`static_fields + 0x08`), проверяем класс объекта и получаем `m_Unmanaged` (`WorldUnmanagedImpl*` по `+0x28`).
   - Обходим блочный массив `SystemState` (`blockBase = [wu + 0x28]`, `sysCount = [wu + 0x48]`, шаг блока `0x290`, в блоке 64 записи по `0xE8` начиная с `[blk + 0x08]`).
   - По совпадению `*(uint32_t*)(ss + 0x40) == typeId` читаем `metaIdx = *(int32_t*)(ss + 0xC8)` (на `alpha`: `meta=104` для `ApplyForceToDeformationSkeletonSystem`, `meta=105` для `BreakPhysicsJointSystem`).

3. **Чтение `OnUpdate` из `Unity.Entities.SystemBaseRegistry`**:
   - Берём `s_Data = *(static_fields + 0x00)`, `infos = [s_Data + 0x10]`, `count = [s_Data + 0x18]`.
   - Читаем `thunk = *(uintptr_t*)(infos + metaIdx * 0x98 + 0x08)`.
   - Если `thunk` лежит вне `lib_burst_generated.dll` (например, `BreakPhysicsJointSystem` в `GameAssembly.dll` на `alpha`), достаточно существующих IL2CPP-хуков.

4. **Разворачивание Burst-переходника (`UnwrapBurstOnUpdateTarget`)**:
   - Внешний thunk (`0x7FFF4076D3F0` на `alpha`) имеет вид:
     ```asm
     55                    push rbp
     48 83 EC 20           sub rsp, 20h
     48 8D 6C 24 20        lea rbp, [rsp+20h]
     FF 15 <disp32>        call qword ptr [rip + disp32]
     ```
   - Разыменование `[thunk + 16 + disp32]` даёт внутренний переходник (`0x7FFF400B8060` на `alpha`):
     ```asm
     55                    push rbp
     48 89 E5              mov rbp, rsp
     5D                    pop rbp
     E9 <rel32>            jmp target
     ```
   - Переход по `E9` выводит на чистый пролог целевой Burst-функции (`0x7FFF400B8070`, `burstRva=0x618070`), куда без ограничений ставится `MinHook` (`Hooked_ApplyForceToDeformationSkeletonBurst` / `Hooked_BreakPhysicsJointBurst`, сигнатура `(void* systemPtr, void* systemState)`), отменяющий выполнение при `ShouldBlockDeformationDamage()`.

---

## 4. Главный верхнеуровневый рубильник (`DeformationPhysicsEnabled` ECS-синглтон)

Даже после блокировки `ApplyForceToDeformationSkeletonSystem::BurstOnUpdate` при столкновении с отбойником капот и крыло продолжали сминаться. Повторный разбор показал, что разработчики **действительно встроили главный рубильник выключения деформации** уровнем выше — через ECS-компонент-синглтон **`CarX.DeformationPhysics.Components.DeformationPhysicsEnabled`**.

### Устройство верхнего уровня:
- **`UI.Controllers.Race.DeformationSystemsController`**:
  - Поля: `+0x18 m_containers`, `+0x20 m_partsDamageModel` (`Races.Models.PartsDamageModel`), `+0x28 m_entityExist`.
  - `OnActivate` читает `m_partsDamageModel.get_enableDamage()`, вызывает `SetDamageSystemsState(bool enable)` и подписывается на `onDamageEnableChanged`.
- **`UI.UIView.Runtime.Controllers.Cutscenes.CutscenesDeformationStateController`**:
  - Содержит статико-совместимый метод `SetDamageSystemsState(bool enable)` (не использует `this`/`rcx`, безопасно вызывается с `nullptr` с главного потока Unity).
  - При `enable == false` находит сущности с `ComponentType.ReadOnly<DeformationPhysicsEnabled>()` и уничтожает их через `BeginSimulationEntityCommandBufferSystem`.
  - При `enable == true` создаёт сущность и добавляет `DeformationPhysicsEnabled`.
- **`Races.Models.PartsDamageModel`** (`+0x10 onDamageEnableChanged`, `+0x18 m_enableDamage`), **`Screens.Runtime.Race.DeformationStateRaceController`** (включает/выключает урон на старте/финише гонки), **`CarX.Game.DR3Console.DR3.Scripts.Base.AlwaysOnCarDamageToggles`** (`get_damageOn` захардкожен в `return 1`).

### Какие 8 ECS-систем требуют `DeformationPhysicsEnabled` (`RequireForUpdate`):
Проверка живого мира через `SystemState::ShouldRunSystem` показала, что при уничтожении синглтона `DeformationPhysicsEnabled` **сразу 8 ECS-систем автоматически переходят в `ShouldRunSystem() == 0`**, включая все оставшиеся системы расчёта балок, скелета и визуальной деформации:
1. `UpdateBeamsSystem` (`sys[28]`, `meta=114`, **Burst** `LBG+0x5F2AA0`) — **именно эта система (вместе с обновлением скелета/behaviours) продолжала сминать капот и крыло об отбойник, пока был перехвачен только `ApplyForce`**.
2. `BreakPhysicsJointSystem` (`sys[43]`, `meta=105`)
3. `UpdateDeformationSkeletonSystem` (`sys[90]`, `meta=116`)
4. `SyncCompoundColliderNodesSystem` (`sys[141]`, `meta=112`, **Burst**)
5. `UpdateJointPositionSystem` (`sys[191]`, `meta=117`, **Burst** `LBG+0x617150`)
6. `ApplyForceToDeformationSkeletonSystem` (`sys[219]`, `meta=104`, **Burst** `LBG+0x618070`)
7. `DeformationSkeletonSleepSystem` (`sys[260]`)
8. `UpdatePhysicsBehavioursSystem` (`sys[292]`, `meta=118`)

При этом остальные системы (`UpdatePhysicsCollidersSystem`, `UpdatePhysicsRootPlaneSystem`, `CreatePhysicsRootColliderSystem`, `UpdateDeformationBehavioursSystem` и т.д.) продолжают работать (`ShouldRunSystem() == 1`), благодаря чему не ломается твёрдотельная физика контакта кузова (wall-tap) и кнопка починки `FullCarRepair` (`il2cpp.repair_car()`).

---

## 5. Итоговое решение в `Urushi` (только главный рубильник)

В [car_sim_optimizer_hooks.cpp](file:///C:/Program%20Files%20(x86)/Steam/steamapps/common/CarX%20Drift%20Racing%20Online%202/Urushi/core/car_sim_optimizer_hooks.cpp) оставлен **только верхнеуровневый главный рубильник (`DeformMaster`)** — без хрупких перехватов внутренних ECS/Burst-систем и без захардкоженных RVA/оффсетов структур `Unity.Entities`:

1. **Перехват контроллеров верхнего уровня**:
   - `CutscenesDeformationStateController::SetDamageSystemsState` и `DeformationSystemsController::SetDamageSystemsState` ищутся динамически через `Il2CppHelpers::LookupTypeClass` / `LookupClassMethod`.
   - Пока активен `ShouldBlockDeformationDamage()`, оригинальное значение от игры запоминается (`s_gameWantsDamage`), а в игру принудительно передаётся `enable = 0`.
2. **Синхронизация синглтона (`SyncDamageMasterSwitch`)**:
   - Вызывается из `SyncStaticSubsystems` строго на главном потоке Unity (`UnityBridge::IsOnMainThread()`).
   - При включении «Без повреждений» или входе в сессию вызывает `CutscenesDeformationStateController::SetDamageSystemsState(nullptr, 0, nullptr)`, уничтожая синглтон `DeformationPhysicsEnabled` (все 8 ECS-систем деформации автоматически переходят в `ShouldRunSystem() == 0`).
   - При выключении «Без повреждений» или `Shutdown()` синглтон аккуратно восстанавливается в исходное состояние (`s_gameWantsDamage` / `1`).
   - (Помимо этого, геометрия колёс/подвески защищена через стабильные C-экспорты `NativeCarX` — `CX_WheelSetDeformOffset` / `CX_WheelSetDeformAngle`.)

---

## 6. Проверка в рантайме (`alpha`, PID 1440)

- После сборки в [Urushi/build_burst_fix](file:///C:/Program%20Files%20(x86)/Steam/steamapps/common/CarX%20Drift%20Racing%20Online%202/Urushi/build_burst_fix) и горячего релоада минималистичной сборки в `urushi.log`:
  - Установлены только два верхних перехвата: `CutscenesDeformationStateController::SetDamageSystemsState`, `DeformationSystemsController::SetDamageSystemsState`.
  - Зафиксировано выключение синглтона при старте: `Player car repair triggered (m_needApplyTuning=1)` и `DeformMaster: disabled DeformationPhysicsEnabled singleton`.
