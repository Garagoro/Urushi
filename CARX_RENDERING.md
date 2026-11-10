# CarX DR2 Rendering & Livery Pipeline (`CarX-WFX` Technical Notes)

Документ фиксирует результаты реверс-инжиниринга рендера машин, материалов и системы ливрей в **CarX Drift Racing Online 2** (Unity `6000.3.19f1`, DirectX 11, IL2CPP + `RenderCore`). По аналогии с `WeaponFX/RENDERING.md`, эти заметки описывают реальное устройство кадра и служат основой для реализации `CarFX`.

*(Все адреса `GameAssembly.dll` (`GA`) и `UnityPlayer.dll` (`UP`) ниже сняты с актуального билда и помечены как `[build-specific]` там, где зависят от патча игры.)*

---

## 1. Главная особенность: как на самом деле рисуется «ливрейный» слой в DR2

В отличие от ожидаемой многослойной схемы («сначала меш базы, поверх отдельные полупрозрачные меши/проходы наклеек»), в основном кадре **CarX DR2 не рисует отдельных слоёв геометрии для винилов**.

Вместо этого:
1. При загрузке или редактировании раскраски `VinylSystem.VinylBaker` заранее **запекает** все слои наклеек на GPU в сжатые текстурные атласы (`2048×2048` для кузова, `1024×1024` для стёкол).
2. Во время основного рендера каждая кузовная деталь рисуется **одним Forward draw call'ом** с материалом `car_body` (`RenderCore/Standard`, keyword `MAT_CAR_PAINT_3_0`).
3. Пиксельный шейдер `MAT_CAR_PAINT_3_0` за один проход сам смешивает базовую краску (`u_vColor * u_sTexture2`) и наклейки (`u_sTexture.rgb`) по альфа-каналу ливреи (`u_sTexture.a`).

---

## 2. Цепочка сборки ливреи и материалов (`VinylSystem` → `CarModelSystem`)

### 2.1. Запекание (`VinylBaker` → `VinylSystem.Livery`)
- `VinylBaker` рендерит винилы через вспомогательные шейдеры `Hidden/VinylSystem/*` (`Clear`, `Livery`, `MaskOnly`, `Dilate`) во временные RenderTarget'ы (`VinylRenderTargetManager`: `m_albedoRT`, `m_material1RT`, `m_material2RT`, `m_geometryMaskRT`).
- Результат сжимается и хранится внутри `VinylSystem.Livery` в двух массивах `CompressedTexture[2]`:
  - `Livery+0x30`: альбедо + альфа покрытия винилов (`[0]` — кузов `2048×2048`, `[1]` — стёкла `1024×1024`);
  - `Livery+0xA0`: PBR-параметры наклеек (`u_sPackedMap`, `2048×2048` и `1024×1024`).
- `Livery.GetTexture(idx)` (`GA+0xD35C10`) пытается достать `RenderTextureReference.TryGet`, иначе возвращает сжатый `Texture2D` из `CompressedTexture+0x20`.

### 2.2. Идентификаторы `CarModelConfig` и `MasterMaterials`
В `CarModelConfig` заданы ключи категорий материалов (`m_masterMaterialAbilityMap`) и ID свойств шейдера `RenderCore/Standard`:

- **TypeID категорий**:
  - `BodyTypeID` = **`0x8C255AA8`** (флаги `0x29`, бит `0x20` = поддержка ливреи, материал `car_body`)
  - `GlassTypeID` = **`0xD9F8C9BF`** (флаги `0x39`, материал `car_glass`)
  - `BodyNoLiveryTypeID` = **`0xA1AF2FAF`** (`0x09`, материал `car_body_nolivery`, keyword `MAT_CAR_PAINT_MINIMAL`)
  - `FrameTypeID` = **`0x963AD5A5`** (`0x09`, материал `car_stock_frame`)
  - `BodyStitchesTypeID` = **`0x4978D52A`** (`0x09`)
- **Shader Property IDs**:
  - `380` (`0x17C`) — **`u_sTexture`** (альбедо ливреи RGB + маска покрытия в `.a`)
  - `1370` (`0x55A`) — **`u_sPackedMap`** (PBR-карта ливреи)
  - `968` (`0x3C8`) — **`u_sTexture2`** (базовая текстура краски / AO)
  - `385 / 386 / 387` — `u_vColor` / `u_vSecondaryColor` / `u_vThirdColor`
  - `382` — `u_fTiling`

### 2.3. Применение на детали кузова и структура в памяти `[build-specific]`
- `CarModelSystem.VinylsModel.ApplyTextures` (`GA+0xD93820`) берёт текстуры из `Livery` (или `Texture2D.blackTexture` для `u_sPackedMap` при `!hasMaterials`) и записывает их в `MasterMaterials.m_textures` (`mm+0x20` — `Dictionary<uint typeId, Dictionary<int propId, Texture>>`) для `0x8C255AA8` и `0xD9F8C9BF`.
- Каждый `CarModelSystem.MaterialPropertyBlockReceiver` на деталях машины (`14` кузовных мешей `car_body` и `4` стеклянных `car_glass` из `308` `MeshRenderer` на тестовой `Kyoto S90_0`) применяет цвета, параметры и текстуры `MasterMaterials` через инстанцированный `car_body(Clone)` и `MaterialPropertyBlock`.
- Верифицированная цепочка указателей от корня машины игрока (`Transform` от `UnityBridge::FindPlayerCarModel`):
  1. `rootGo = *(Transform* + 0x20)` (нативное поле `m_GameObject` в `UnityPlayer.dll`)
  2. Массив компонентов `GameObject`: `compArr = *(rootGo + 0x20)`, `compCount = *(uint32_t*)(rootGo + 0x30)`; записи по `16` байт (`uint32_t classId` по `+0x00`, `void* nativeComp` по `+0x08`).
  3. Для `classId == 66` (`MonoBehaviour`): GCHandle слот по `slot = *(nat + 0x18)`, управляемый объект `obj = *(slot & ~7)`. Класс `(*(uintptr_t*)obj)`: `+0x10` — имя (`"CarBehaviour"`), `+0x18` — namespace (`"Framework.Views.Cars"`).
  4. От `CarBehaviour`:
     - `cb + 0x60` → `CarModelSystem.CarModel`
     - `cb + 0x68` → `CarModelSystem.VinylsModel`
  5. От `CarModel`:
     - `cm + 0x38` → `CarModelSystem.PartsMap`
  6. От `PartsMap`:
     - `pm + 0x18` → `CarModelSystem.MasterMaterials`
  7. От `MasterMaterials`:
     - `mm + 0x20` → `Dictionary<uint, Dictionary<int, Texture>>` (`+0x18` массив `Entry[]`, где `count` по `+0x20`; размер `Entry` = 24 байта: `hashCode@0`, `next@4`, `key@8`, `value@16`).
  8. Native `Texture*` = `*(managedTex + 0x10)`.
  9. Преобразование native `Texture*` → D3D11 указатель: через Unity icall `UnityEngine.Texture::GetNativeTexturePtr_Injected(System.IntPtr)` (`UP+0xA8710` → `UP+0x4824F0` → `GfxDeviceD3D11+0x820`, также резолвится через `GA+0x926750` `il2cpp_resolve_icall`). В C++ проверяем возвращённый COM-объект через `QueryInterface` на `ID3D11ShaderResourceView` / `ID3D11Texture2D`.

---

## 3. Конвейер `RenderCore` и как кадр уходит в DX11

### 3.1. Порядок проходов кадра
- В `RenderCore.RenderPipeline.RenderFrame` (`GA+0x50F1120`) в обычном режиме (`(frameData+0xE8 & 0x20) == 0`) всегда запускается **`RecordFullRenderLoop`** (`GA+0x50E9850`).
- Порядок проходов в `RenderGraph`:
  1. `ShadowPass` (`SHADOWCASTER`)
  2. `DepthPrePass` (`DepthOnly`)
  3. `ContactShadowsPass` / `AmbientOcclusionPass` / `MotionVectorsPass` (`MOTIONVECTORS`) / `SSR` / `MirrorReflectionsPass`
  4. **`ForwardPass`** (`Forward` / `ForwardGhost`)
  5. `GhostObjectsPass` → `ParticleEffectSystem` → `PostProcessSystem` → `OutlinePass` → `LateForwardPass`
- `RenderCore/Standard` содержит **6 проходов** (`Forward`, `ForwardGhost`, `SHADOWCASTER`, `DepthOnly`, `MOTIONVECTORS`, `TexelDensity`) и **не имеет `GBuffer`-прохода**.
- Очередь рендера (`RenderQueue`): `car_body` и `car_body_nolivery` = **`2200`**, `car_glass` = **`2600`**.
- В `ForwardPass.RenderForwardPass` (`GA+0x504F9A0`) сначала идёт `IndirectDrawer.Draw` (если активен), затем цикл отправки `RendererList` в `ScriptableRenderContext`.

### 3.2. Отличие проходов и скомпилированные DXBC-шейдеры `RenderCore/Standard`
- **Отсев глубины и теней**:
  - `DepthOnly` / `ShadowCaster` (обычный) имеет пустой PS (0 инструкций, `ret`, без выходов и **0 текстур**).
  - `DepthOnly` с keyword `NORMALS_BUFFER` пишет октаэдральную нормаль в `o0.xy` и использует только `cb0[3]` — **0 текстур**.
  - Текстуры материала привязаны **только в `Forward` (и `ForwardGhost`)** с единым HDR-выходом **`SV_Target0` (`float4 o0.xyzw`)**.

- **Точная входная сигнатура PS (`MAT_CAR_PAINT_3_0`)**:
  ```hlsl
  struct PSIn {
      float2 uv        : TEXCOORD20;   // v0.xy
      float  vcolW     : TEXCOORD21;   // v0.z (Vertex COLOR.w)
      float  vcolX     : TEXCOORD22;   // v0.w (Vertex COLOR.x)
      float3 posRelCam : TEXCOORD0;    // v1.xyz (WorldPos - CameraPos)
      float3 normalW   : TEXCOORD1;    // v2.xyz (World Normal)
      float4 tangentW  : TEXCOORD2;    // v3.xyzw (xyz + bitangent sign)
      float4 svpos     : SV_Position;  // v4.xy
  };
  ```
  > В `MAT_CAR_PAINT_MINIMAL` отсутствует `TEXCOORD22`, а в `MAT_CAR_GLASS` нет `TEXCOORD21`/`TEXCOORD22`, но есть `SV_IsFrontFace`. Повторно использовать один и тот же `PSIn` можно на всех деталях с `MAT_CAR_PAINT_3_0` (весь кузов `car_body`).

- **Важное открытие — плавающие номера `t#` слотов**:
  - Номера SRV-регистров `u_sTexture` и `u_sPackedMap` **меняются** в зависимости от активных shader keyword'ов графики (`SHADING_MQ`/`SHADING_LQ`/`VLQ`, `APPLY_SSAO`, `APPLY_CONTACT_SHADOWS`, `DOTS_INSTANCING_ON`):
    - Базовый (`Chunk 4`): `u_sTexture = t15`, `u_sPackedMap = t16`, `u_sNormalMap = t20`, `u_sTexture2 = t21`.
    - Средние/низкие/SSAO/DOTS варианты: `u_sTexture` смещается на `t17`, `t19`, `t20` или `t21` (а `u_sPackedMap` следует сразу за ним на `t+1`).
  - Максимальный использованный слот игры среди всех исследованных вариантов — `t28`. **Слоты `t32..t41` полностью свободны**.
  - `cb0..cb11` заняты игрой (`cb6` — `UnityPerMaterial`: `u_vColor` в `cb6[0]`, `u_vSecondaryColor` в `cb6[1]`, `u_fParams` в `cb6[4]`; `cb5` — viewport/screen). **Слоты `b12` и `b13` полностью свободны**.

---

## 4. Адаптация механики `WeaponFX` под CarX DR2 (`CarFX`)

1. **Детекция только своей машины (парный SRV + RT-фильтр)**:
   - В `DrawIndexed` / `DrawIndexedInstanced` сканируем `PSGetShaderResources(12, 20, srvs)` (`t12..t31`).
   - Срабатываем **только если в слотах одновременно присутствуют оба SRV кузова игрока** — `srvAlbedo` (`u_sTexture`) и `srvPacked` (`u_sPackedMap`) (а если одна из них отсутствует/равна дефолту, проверяем хотя бы уникальный не-дефолтный SRV + цветной RTV).
   - Проверка **двух SRV подряд** решает баг «протекания слота» D3D11 (описанный в `WeaponFX/RENDERING.md` §2.1): если следующий draw call другого объекта не перепривязал один слот, пара `(srvAlbedo, srvPacked)` уже не сойдётся.
   - **Фильтр RenderTarget**: через `OMGetRenderTargets` проверяем наличие активного `RTV[0]` и соответствие его размеров размерам основного вьюпорта (`Width >= vp.Width * 0.5f`), чтобы не задевать мелкие пробы отражений/кубмап.
   - **Кейс сток-машины без винилов**: если у игрока на кузове нет ни одной наклейки и `u_sTexture` / `u_sPackedMap` указывают на системные `Texture2D.whiteTexture` / `blackTexture` (общие с другими сток-машинами), нужно не полагаться на дефолтную текстуру, а подменять/создавать локальной машине уникальную `4×4` маркер-текстуру (или отслеживать `car_body(Clone)`).

2. **Особенности перехвата D3D11 на Windows 11**:
   - Как выяснено в `WeaponFX` (§1.4), `d3d11.dll` может переключать vtable immediate-контекста во время работы и вызывать nested draw-функции. Поэтому используем:
     - **MinHook** (`HookManager`) на `DrawIndexed` (`vtbl[12]`) и `DrawIndexedInstanced` (`vtbl[20]`) — как на `dummyContext` при инициализации, так и с дохукиванием vtable реального `s_context` в `Present`, если адреса thunks сменились;
     - **Thread-local/atomic `Guard` по `GetCurrentThreadId()`**: любой вложенный D3D-вызов изнутри нашего хука уходит напрямую в `original`.

3. **Доступ к запечённой ливрее в шейдере (`g_fxLivery`) и режимы**:
   - Перед подменой PS перехваченный `srvAlbedo` биндится в **`t38` (`g_fxLivery`)**, поэтому пользовательский `.hlsl` может в любой момент:
     - читать цвета наклеек: `g_fxLivery.Sample(g_linear, i.uv).rgb`;
     - маскироваться строго по зоне винилов (`livery.a`) или строго по чистой краске (`1 - livery.a`);
     - работать в `@mode replace` (полная замена шейдера с сохранением теней/глубины сцены) или `@mode overlay` (поверх оригинального `MAT_CAR_PAINT_3_0` с `AlphaBlend` и `DepthFunc = LESS_EQUAL`, `DepthWrite = ZERO`);
     - читать копию фона (`g_fxScene` `t32`), буфер глубины (`g_fxDepth` `t37`), пользовательские маски (`g_fxTex0..3` `t33..t36`), параметры `@param` / `@color` и телеметрию машины `g_fxGame[4]` в `b13`.
