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
