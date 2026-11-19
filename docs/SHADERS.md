# CarFX Shader Documentation & API

**CarFX** is a real-time DirectX 11 HLSL shader injection pipeline built into URUSHI for **CarX Drift Racing Online 2**. It intercepts and overlays or replaces custom pixel shaders strictly on the player car's body (`MAT_CAR_PAINT_3_0` Forward pass).

All shaders are located in the `shaders/` directory as standard `.hlsl` files.

> **Instant Hot-Reload:** You do not need to restart the game or recompile shaders manually. As soon as you save any `.hlsl` file inside `shaders/`, URUSHI detects the change, recompiles it via D3DCompile, and swaps the shader pipeline on the fly!

---

## Table of Contents
1. [Shader File Structure](#1-shader-file-structure)
2. [Header Annotations (Auto-Generated UI)](#2-header-annotations-auto-generated-ui)
3. [Input Registers & Structures](#3-input-registers--structures)
4. [CarFX Constant Buffer (`b13`)](#4-carfx-constant-buffer-b13)
5. [Textures & Samplers](#5-textures--samplers)
6. [Built-in Helper Functions](#6-built-in-helper-functions)
7. [Step-by-Step Example: Reactive Drift Shader](#7-step-by-step-example-reactive-drift-shader)
8. [Internals target (engine, suspension, interior)](#8-internals-target-engine-suspension-interior)
9. [Post-Process Shaders (`shaders/post/*.hlsl`)](#9-post-process-shaders-shadersposthlsl)

---

## 1. Shader File Structure

Every CarFX shader starts with `#include "carfx.hlsli"` and defines a `main` pixel shader entry point:

```hlsl
// @mode replace
// @param glowIntensity p0.x 0.0..5.0 = 1.5 : Glow intensity
// @color accentColor   p1.xyz = 1.0 0.4 0.1 : Accent neon color
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float fresnel = Fresnel(i, 3.0);

    float3 col = p1.xyz * fresnel * p0.x;
    return float4(col, 1.0);
}
```

---

## 2. Header Annotations (Auto-Generated UI)

URUSHI parses comment annotations at the top of `.hlsl` files and **automatically builds UI sliders and color pickers** in the **CarFX** menu.

### `@mode`
Specifies how the shader outputs blend with the vehicle:
* `// @mode replace` — Replaces the car body surface completely with the shader output.
* `// @mode overlay` — Blends on top of the original car paint.

### `@param`
Exposes float slider controls:
```hlsl
// @param <name> <slot>.<swizzle> <min>..<max> = <default> : <Description>
```
* **Slot:** `p0` through `p7`.
* **Swizzle:** `.x`, `.xy`, `.xyz`, `.xyzw`.
* **Example:**
  ```hlsl
  // @param pulseSpeed p0.x 0.1..10.0 = 2.0 : Speed of color pulsation
  // @param waveScale  p0.y 1.0..50.0 = 10.0 : UV wave frequency
  ```

### `@color`
Exposes an interactive RGB / RGBA color picker with square SV, Hue slider, and Hex input:
```hlsl
// @color <name> <slot>.<swizzle> = <r> <g> <b> [<a>] : <Description>
```
* **Example:**
  ```hlsl
  // @color primaryGlow   p1.xyz = 0.2 0.8 1.0 : Primary neon tint
  // @color secondaryGlow p2.xyzw = 1.0 0.1 0.5 1.0 : Secondary rim tint
  ```

### `@tex`
Binds custom texture assets:
```hlsl
// @tex <slot 0..3> <name> [coverage | colour] : <Description>
```
* Binds to registers `g_fxTex0` (`t33`) through `g_fxTex3` (`t36`).
* **Supported sources** (confined strictly to `shaders/`):
  * **Static images**: `.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga`.
  * **Animated GIF**: `.gif` — full GIF89a composite with frame rects, per-frame delays (delays $\le 10\text{ ms}$ normalized to $100\text{ ms}$), and disposal modes 0..3.
  * **Image sequence folders**: `shaders/<dir>/` containing numbered `.png`/`.jpg` frames sorted naturally, running at a configurable FPS (default 24 FPS; optional `fps.txt` in the folder).
* **Budgets & Performance**:
  * Decoded entirely on background worker threads; no disk I/O or decode stalls on the render thread.
  * Long-side resolution downscaled to $\le 1024\text{ px}$ by default (hard max $2048\text{ px}$).
  * Total decoded memory capped at $256\text{ MB}$ per clip; clips exceeding budget are automatically downscaled / subsampled with a warning log.
  * Maximum frame count capped at 600 frames.
  * Swapped on the GPU as immutable textures on `OnNewFrame` with zero per-frame CPU upload.
  * Playback status exposed in the UI (play/pause, speed, loop/ping-pong/once, frame scrubber).

### `@stealth`
```hlsl
// @stealth 1
```
Hides internal default parameters from the UI panel to keep the interface clean.

---

## 3. Input Registers & Structures

The `PSIn` structure matches CarX's native vertex shader outputs:

```hlsl
struct PSIn {
    float2 uv        : TEXCOORD20;   // Mesh / Livery UV0
    float  vcolW     : TEXCOORD21;   // Vertex Color W
    float  vcolX     : TEXCOORD22;   // Vertex Color X
    float3 posRelCam : TEXCOORD0;    // World position relative to camera (worldPos - camPos)
    float3 normalW   : TEXCOORD1;    // World-space surface normal
    float4 tangentW  : TEXCOORD2;    // World-space tangent (xyz) + sign (w)
    float4 svpos     : SV_Position;  // Screen-space pixel coordinates
};
```

---

## 4. CarFX Constant Buffer (`b13`)

All real-time environment, telemetry, and sensor data is pushed into `cbuffer CarFx : register(b13)`:

### Time & View
* `g_fxTime.x` — Running time in seconds.
* `g_fxTime.y` — Frame delta time.
* `g_fxScreen.xy` — Viewport width & height in pixels.
* `g_fxScreen.zw` — Inverse viewport size (`1.0 / width`, `1.0 / height`).

### Sliders & Colors (`g_fxParam[8]`)
* Parameters defined via `@param` and `@color` map into `g_fxParam[0]` (`p0`) through `g_fxParam[7]` (`p7`).
* You can access them directly as `p0.x`, `p1.xyz`, etc.

### Car Transforms & Geometry
* `g_fxCarPos.xyz` — Player car center in world coordinates.
* `g_fxCarPos.w` — Half-width of car (~`0.95m`).
* `g_fxCarRight.xyz` — Local car Right vector (+X).
* `g_fxCarUp.xyz` — Local car Up vector (+Y).
* `g_fxCarFwd.xyz` — Local car Forward vector (+Z).
* `g_fxCamPos.xyz` — Main camera position in world space.

### Proximity & Tandem Sensors
* `g_fxNearCarPos.xyz` — World position of the nearest tandem car or ghost car.
* `g_fxNearCarPos.w` — Distance to nearest car in meters (values `> 900` mean no car nearby).
* `g_fxNearCarClosest.xyz` — Exact closest surface point on the other car's bounding box.

### Wall Proximity & Raycasting
* `g_fxWallDirDist.x` — Distance to wall in front (meters).
* `g_fxWallDirDist.y` — Distance to wall behind (meters).
* `g_fxWallDirDist.z` — Distance to wall on left side.
* `g_fxWallDirDist.w` — Distance to wall on right side.
* `g_fxWallNearestHit.xyz` — Closest wall contact point.
* `g_fxWallNearestHit.w` — Minimum wall distance across all directions.
* `g_fxWallNearestNormal.xyz` — Normal vector of the nearest wall surface.

### Live Drift Metrics
* `g_fxDriftState.x` — Absolute drift angle in degrees (`0..180°`).
* `g_fxDriftState.y` — Signed drift angle (`-180..+180°`, negative = left, positive = right).
* `g_fxDriftState.z` — Normalized drift angle (`0.0` at 0°, `1.0` at 65°).
* `g_fxDriftState.w` — Car speed in km/h.

### Music Reactive Audio Inputs (App & System Loopback)
* `g_fxAudio.x` — Overall RMS volume level (`0.0..1.0`).
* `g_fxAudio.y` — Bass frequency band energy (`20..250 Hz`, `0.0..1.0`).
* `g_fxAudio.z` — Combined mid frequency band energy (`250..4000 Hz`, `0.0..1.0`).
* `g_fxAudio.w` — Treble frequency band energy (`4000..16000 Hz`, `0.0..1.0`).
* `g_fxAudio2.x` — Beat onset pulse (`1.0` on bass onset/kick, decaying smoothly to `0.0`).
* `g_fxAudio2.y` — Low-mid band energy (`250..1000 Hz`).
* `g_fxAudio2.z` — High-mid band energy (`1000..4000 Hz`).
* `g_fxAudio2.w` — Audio capture enabled flag (`1.0` if active, `0.0` if off).
* `g_fxAudioSpectrum[8]` — 32 log-spaced spectrum bands (`float4[8]`). Zeroed when capture is off.

### Animated Texture Metrics (`g_fxTexAnim[4]`)
Status for texture slots 0..3 (`g_fxTex0`..`g_fxTex3`):
* `g_fxTexAnim[slot].x` — Normalized playback progress (`0.0 .. 1.0`).
* `g_fxTexAnim[slot].y` — Current frame index (0-based).
* `g_fxTexAnim[slot].z` — Total frame count.
* `g_fxTexAnim[slot].w` — Playing state (`1.0` = playing, `0.0` = paused / static).

---

## 5. Textures & Samplers

CarFX binds dedicated textures on registers `t32` to `t38`:

```hlsl
Texture2D g_fxScene  : register(t32);   // Background scene snapshot behind the car
Texture2D g_fxTex0   : register(t33);   // User texture 0
Texture2D g_fxTex1   : register(t34);   // User texture 1
Texture2D g_fxTex2   : register(t35);   // User texture 2
Texture2D g_fxTex3   : register(t36);   // User texture 3
Texture2D g_fxDepth  : register(t37);   // Depth buffer snapshot before car
Texture2D g_fxLivery : register(t38);   // Baked 2048x2048 player livery atlas

SamplerState g_linear : register(s14);  // Linear filter, wrap sampler
SamplerState g_point  : register(s15);  // Point filter, clamp sampler
```

### Preserving Player Liveries
To retain the car's existing decals and vinyls on top of your shader:
```hlsl
float4 livery = g_fxLivery.Sample(g_linear, i.uv);
// livery.rgb = decal colors, livery.a = decal opacity
float3 finalColor = lerp(shaderOutput, livery.rgb, livery.a);
```

---

## 6. Built-in Helper Functions

Included in `carfx.hlsli`:

* `float3 Normal(PSIn i)` — Returns normalized world normal `normalize(i.normalW)`.
* `float3 ViewDir(PSIn i)` — Returns normalized direction to camera `normalize(-i.posRelCam)`.
* `float Fresnel(PSIn i, float power)` — Calculates standard Schlick Fresnel term.
* `float2 ScreenUV(PSIn i)` — Returns normalized screen UVs `[0..1]`.

### Audio Reactive Helpers
* `bool AudioEnabled()` — Returns `true` if audio capture is active and feeding data.
* `float AudioLevel()` — Overall volume level `[0..1]`.
* `float AudioBass()` — Bass pulse energy `[0..1]`.
* `float AudioMid()` — Mid-frequency energy `[0..1]`.
* `float AudioTreble()` — Treble frequency energy `[0..1]`.
* `float AudioBeat()` — Beat onset strobe pulse (jumps to `1.0` on drum/kick hits and decays).
* `float AudioLowMid()`, `float AudioHighMid()` — Fine frequency ranges.
* `float AudioSpectrumBand(uint band)` — Magnitude of band `0..31`.
* `float AudioSpectrum(float u)` — Linearly interpolated continuous sampling across the 32 bands (`u in 0..1`). Sample along `CarLocalPos(i).z` to map frequencies across the length of the car!

Sample shader: `shaders/livery_music.hlsl`.

---

## 7. Step-by-Step Example: Reactive Drift Shader

Create `shaders/livery_my_drift.hlsl`:

```hlsl
// @mode replace
// @color basePaint     p0.xyz = 0.05 0.05 0.07 : Dark base paint
// @color neonAngle     p1.xyz = 1.00 0.10 0.35 : High-angle neon glow
// @param glowBoost     p2.x 0.5..5.0 = 2.0     : Overall glow multiplier
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float fresnel = Fresnel(i, 3.5);

    // Get live drift angle normalized (0.0 to 1.0 at 65°+)
    float driftFactor = saturate(g_fxDriftState.z);

    // Dynamic wave moving faster at higher speeds
    float speed = g_fxDriftState.w;
    float wave = sin(i.uv.y * 20.0 + g_fxTime.x * (speed * 0.1 + 1.0)) * 0.5 + 0.5;

    // Blend base paint with drift-reactive neon
    float3 neon = p1.xyz * fresnel * (driftFactor * 1.5 + 0.2) * (wave * 0.5 + 0.5) * p2.x;
    float3 bodyColor = p0.xyz + neon;

    // Overlay player's baked livery decals
    float4 livery = g_fxLivery.Sample(g_linear, i.uv);
    float3 finalColor = lerp(bodyColor, livery.rgb, livery.a);

    return float4(finalColor, 1.0);
}
```

Save the file, open CarFX in the URUSHI menu, select **`livery_my_drift`**, and watch your car illuminate in tandem drifts!

---

## 8. Internals target (engine, suspension, interior)

CarFX has a second slot for the **non-body parts of the player car**. Load any shader into it from *CarFX → Internals*
(or `carfx.load("my_shader.hlsl", 1)`); it uses the same `PSIn`, `b13` buffer and helpers as a body shader, so existing examples work.

* Draws are recognised by the albedo textures of the car's non-body parts (wheels, brakes, glass and shadow proxies are excluded).
  Parts whose material only has a default (white/black) texture cannot be told apart and keep their normal look.
* Telemetry / spatial data (`g_fxGame`, `g_fxCar*`, `g_fxCam*`) are the player car's.
* `g_fxLivery` is not bound for this target (no livery on internals).
* Parts: pick which groups it applies to in the CarFX window or with `carfx.set_parts{...}` (engine, suspension, brakes, wheels, interior). The body is never split.
* Sample: `shaders/internals_neon.hlsl`.

---

## 9. Post-Process Shaders (`shaders/post/*.hlsl`)

URUSHI includes a full-screen post-processing pipeline (`core/postfx.*`) that executes on the render thread inside `IDXGISwapChain::Present` immediately before ImGui is rendered. This guarantees that full-screen effects never distort or occlude the URUSHI menu or HUD widgets.

All post-process shaders must be placed in `shaders/post/` with the `.hlsl` extension.

### Execution Model
1. The game's backbuffer is resolved/copied into a temporary staging texture.
2. A single full-screen triangle is drawn via a built-in procedural vertex shader (`SV_VertexID`), producing pixel UV coordinates across `[0..1]`.
3. The custom pixel shader samples the scene color (and optional depth), processes the image, and writes directly into the swapchain render target.
4. All Direct3D 11 pipeline state (viewports, rasterizer, blend state, depth-stencil state, shader bindings) is saved and restored around the pass.

### Constant Buffers (`b0` and `b13`)
Post-process pixel shaders receive a constant buffer bound to registers `b0` and `b13`:
```hlsl
cbuffer PostFXConstantBuffer : register(b0) {
    float4 g_fxTime;         // (time_sec, delta_time, 0, 0)
    float4 g_fxScreen;       // (width, height, 1.0/width, 1.0/height)
    float4 g_fxParam0;       // p0 (.xyzw)
    float4 g_fxParam1;       // p1
    float4 g_fxParam2;       // p2
    float4 g_fxParam3;       // p3
    float4 g_fxParam4;       // p4
    float4 g_fxParam5;       // p5
    float4 g_fxParam6;       // p6
    float4 g_fxParam7;       // p7
    float4 g_fxDriftState;   // (abs_angle_deg, signed_angle_deg, norm_factor_0_to_1, speed_kmh)
};
```

### Textures & Samplers
- **Scene Texture:** Register `t0` and `t32` (`Texture2D g_sceneTex`). Contains the captured game backbuffer color.
- **Depth Texture:** Register `t1` and `t37` (`Texture2D g_depthTex`). Contains the captured scene depth buffer (when CarFX depth capture is active).
- **Linear Sampler:** Register `s0` and `s14` (`SamplerState g_linear`).
- **Point Sampler:** Register `s1` and `s15` (`SamplerState g_point`).

### Input Structure & Header Annotations
Post-process shaders use the following pixel shader input:
```hlsl
struct PSIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
```

Header annotations (`@param` and `@color`) are parsed identically to CarFX shaders, allowing scripts to inspect parameter ranges and build dynamic UI controls:
```hlsl
// @param intensity p0.x 0.0..1.0 = 0.0 : Effect intensity
// @param lineCount p0.y 10.0..100.0 = 40.0 : Density of lines
// @color tintColor p1.xyz = 1.0 1.0 1.0 : Line tint color
```

Sample post-process shader: `shaders/post/speedlines.hlsl` (Initial D anime radial speed lines).

---

## 10. Animated Video Livery (`shaders/livery_video.hlsl`)

Maps animated GIF or image sequence textures from user texture slot 0 onto the car body with real-time UV transformation, brightness, emissive boost, and optional preservation of the player's decals:

* `@param uvScaleX` (`p0.x`), `@param uvScaleY` (`p0.y`): UV tiling / scale.
* `@param uvOffsetX` (`p0.z`), `@param uvOffsetY` (`p0.w`): Horizontal and vertical UV offset.
* `@param uvRotation` (`p1.x`): Texture rotation in degrees (-180°..+180°).
* `@param wrapMode` (`p1.y`): `1.0` = Repeat / Tile, `0.0` = Clamp with edge cutoff.
* `@param brightness` (`p1.z`): Albedo brightness scale.
* `@param emissiveBoost` (`p1.w`): Glowing neon intensity.
* `@param paintBlend` (`p2.x`): Blends video output with base car paint (`0.0` = full video, `1.0` = full paint).
* `@param keepDecals` (`p2.y`): Blends the player's baked vinyl decals and livery stickers on top.
* `@color basePaint` (`p3.xyz`): Base body color shown underneath clamped borders or blended.
