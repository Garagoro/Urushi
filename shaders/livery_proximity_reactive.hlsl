// Proximity Reactive Armor: Simultaneous Front/Door/Rear Wall-Tap + Tandem Glow + Two-Tone Split.
// Reacts in real time (without snapping or speed-coupled phase jitter) to:
//   - Front / Rear bumpers & corners, AND Left / Right doors (between front & rear wheels)
//   - Multiple active contact points simultaneously (e.g. Front + Door + Rear all light up together)
//   - Smooth spatial flow / fade controlled by flowSpeed slider
//   - Another vehicle (or 3D ghost) closing in for door-to-door tandems (true OBB surface SDF)
// @param wallRange  p0.x 0.5..10.0 = 3.8  : wall detection radius (meters)
// @param carRange   p0.y 0.5..10.0 = 3.5  : tandem car proximity radius (meters)
// @param pulseSpeed p0.z 0.0..8.0  = 2.6  : wave travel speed (independent of car speed)
// @param glowPower  p0.w 0.2..6.0  = 3.0  : HDR proximity glow intensity
// @param flowSpeed  p5.x 0.5..30.0 = 6.0  : smooth transition / flow speed (lower = softer flow)
// @color wallCol    p1.xyz = 1.0 0.34 0.03 : wall-tap / barrier proximity neon colour
// @param splitSides p1.w 0.0..1.0  = 0.55 : mix split left/right side base colours
// @color tandemCol  p2.xyz = 0.02 0.90 1.0 : tandem car proximity plasma colour
// @param keepLivery p2.w 0.0..1.0  = 0.85 : preserve game vinyl decals cleanly
// @color leftCol    p3.xyz = 0.05 0.14 0.32 : left side paint tint
// @param gridScale  p3.w 0.5..6.0  = 2.6  : local metric energy ring & grid frequency
// @color rightCol   p4.xyz = 0.24 0.06 0.16 : right side paint tint
// @param fresnelPow p4.w 0.5..6.0  = 2.8  : clearcoat rim light sharpness
// @mode replace
#include "carfx.hlsli"

float WallSpotMask(PSIn i, float4 wallHit, float maxRange, float timeVal, float ringFreq) {
    if (wallHit.w >= maxRange || !HasCarSpatial()) return 0.0;
    float gap = saturate(1.0 - wallHit.w / maxRange);
    gap = pow(gap, 1.5);

    float dPx = CarPixelToWallHitDist(i, wallHit);
    float dSurf = max(dPx - wallHit.w, 0.0);
    // Keep glow localized to this bumper/door/corner while smoothly spreading outward on close approach
    float radius = lerp(0.95, 0.58, gap);
    float radial = exp(-pow(dSurf / max(radius, 0.25), 2.0) * 2.3);

    // IMPORTANT: Keep time frequency constant (no dynamic gap*timeVal term!) so car motion never speeds up animation
    float wave = 0.5 + 0.5 * sin(dSurf * (5.2 * ringFreq) - timeVal * 2.6);
    float ripple = pow(wave, 3.0) * exp(-dSurf * 1.5);

    // Hot contact flare when within ~0.8m of a wall/guardrail
    float tapFlare = pow(saturate(1.0 - wallHit.w / 0.85), 1.45) * exp(-dSurf * dSurf * 2.4);
    return saturate(gap * (radial * 0.82 + ripple * 0.65) + tapFlare * 1.25);
}

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);
    float fres = Fresnel(i, g_fxParam[4].w);

    float wallRange  = max(g_fxParam[0].x, 0.25);
    float carRange   = max(g_fxParam[0].y, 0.25);
    float speed      = g_fxParam[0].z;
    float glowPow    = g_fxParam[0].w;
    float ringFreq   = g_fxParam[3].w;
    float t          = g_fxTime.x * speed;

    // Local coordinates (available whenever MainThread camera + car transform synced)
    float3 lp = CarLocalPos(i);
    float3 ln = CarLocalNormal(i);

    // 1. Glossy two-tone metallic paint (left vs right side in vehicle-local space)
    float sideSplit = HasCarSpatial() ? smoothstep(-0.14, 0.14, lp.x) : 0.5;
    float3 baseL = g_fxParam[3].xyz;
    float3 baseR = lerp(baseL, g_fxParam[4].xyz, g_fxParam[1].w);
    float3 baseTint = lerp(baseL, baseR, sideSplit);

    float horizon = smoothstep(-0.15, 0.25, r.y);
    float3 skyEnv = lerp(float3(0.06, 0.09, 0.15), float3(0.68, 0.82, 1.00), pow(saturate(r.y * 0.5 + 0.5), 1.35));
    float3 gndEnv = lerp(float3(0.015, 0.018, 0.024), float3(0.08, 0.09, 0.12), saturate(-r.y));
    float sunSpec = pow(saturate(dot(r, normalize(float3(0.45, 0.82, -0.35)))), 64.0) * 1.7;
    float3 paint = lerp(gndEnv, skyEnv, horizon) * (baseTint * 1.9 + 0.03) + baseTint * 0.52;
    paint += float3(0.92, 0.96, 1.0) * (sunSpec * (0.35 + 0.65 * fres));

    // Subtle metric hex/panel grid locked to car-local meters (appears inside energized zones)
    float2 mGrid = abs(frac(float2(lp.z, lp.x + lp.y) * ringFreq) - 0.5) * 2.0;
    float gridLine = smoothstep(0.87, 0.99, max(mGrid.x, mGrid.y));

    // 2. Simultaneous per-zone wall proximity (Front, Rear, Left/Right Doors between wheels, and 4 Corners)
    float hw = max(g_fxCarPos.w, 0.85);
    float hl = max(g_fxCarRight.w, 1.80);
    float lxN = lp.x / hw;
    float lzN = lp.z / hl;

    // Front & Rear bumpers
    float wFront = WallSpotMask(i, g_fxWallFrontHit, wallRange, t, ringFreq)
                 * smoothstep(-0.05, 0.45, lzN)
                 * lerp(0.30, 1.0, smoothstep(-0.20, 0.35, ln.z));

    float wRear  = WallSpotMask(i, g_fxWallRearHit, wallRange, t, ringFreq)
                 * smoothstep(-0.05, 0.45, -lzN)
                 * lerp(0.30, 1.0, smoothstep(-0.20, 0.35, -ln.z));

    // Left & Right Doors (focused between front & rear wheel arches)
    float doorBelt = 1.0 - smoothstep(0.42, 0.72, abs(lzN));
    float wDoorL = WallSpotMask(i, g_fxWallDoorLeftHit, wallRange, t, ringFreq)
                 * smoothstep(-0.12, 0.35, -lxN)
                 * lerp(0.20, 1.0, smoothstep(-0.25, 0.30, -ln.x))
                 * lerp(0.35, 1.0, doorBelt);

    float wDoorR = WallSpotMask(i, g_fxWallDoorRightHit, wallRange, t, ringFreq)
                 * smoothstep(-0.12, 0.35, lxN)
                 * lerp(0.20, 1.0, smoothstep(-0.25, 0.30, ln.x))
                 * lerp(0.35, 1.0, doorBelt);

    // Front & Rear Left/Right fenders and corners
    float wFrontL = WallSpotMask(i, g_fxWallFrontLeftHit, wallRange, t, ringFreq)
                  * smoothstep(-0.12, 0.35, -lxN)
                  * smoothstep(-0.05, 0.45, lzN)
                  * lerp(0.22, 1.0, smoothstep(-0.25, 0.30, max(-ln.x, ln.z)));

    float wFrontR = WallSpotMask(i, g_fxWallFrontRightHit, wallRange, t, ringFreq)
                  * smoothstep(-0.12, 0.35, lxN)
                  * smoothstep(-0.05, 0.45, lzN)
                  * lerp(0.22, 1.0, smoothstep(-0.25, 0.30, max(ln.x, ln.z)));

    float wRearL  = WallSpotMask(i, g_fxWallRearLeftHit, wallRange, t, ringFreq)
                  * smoothstep(-0.12, 0.35, -lxN)
                  * smoothstep(-0.05, 0.45, -lzN)
                  * lerp(0.22, 1.0, smoothstep(-0.25, 0.30, max(-ln.x, -ln.z)));

    float wRearR  = WallSpotMask(i, g_fxWallRearRightHit, wallRange, t, ringFreq)
                  * smoothstep(-0.12, 0.35, lxN)
                  * smoothstep(-0.05, 0.45, -lzN)
                  * lerp(0.22, 1.0, smoothstep(-0.25, 0.30, max(ln.x, -ln.z)));

    // Smooth screen-blend allows ALL approached points (e.g., Front + Door + Rear) to glow simultaneously
    float wallReact = saturate(
        1.0
        - (1.0 - wFront)  * (1.0 - wRear)
        * (1.0 - wDoorL)  * (1.0 - wDoorR)
        * (1.0 - wFrontL) * (1.0 - wFrontR)
        * (1.0 - wRearL)  * (1.0 - wRearR)
    );

    // 3. Nearest car / 3D ghost OBB distance reaction (door-to-door tandem glow)
    float dCarPx = CarPixelToNearCarDist(i);
    float carReact = 0.0;
    float carArc   = 0.0;
    if (dCarPx < carRange && HasCarSpatial()) {
        float cNorm = saturate(1.0 - dCarPx / carRange);
        float cWave = 0.5 + 0.5 * sin(dCarPx * (6.0 * ringFreq) - t * 3.0);
        carReact = pow(cNorm, 1.75) * (0.65 + 0.55 * pow(cWave, 2.5));
        carArc   = pow(saturate(1.0 - dCarPx / max(carRange * 0.48, 0.4)), 2.0) * (0.30 + 0.70 * gridLine);
    }

    // 4. Combine reactive glows + crisp silhouette rim
    float3 wallCol   = g_fxParam[1].xyz;
    float3 tandemCol = g_fxParam[2].xyz;
    float totalReact = saturate(max(wallReact, carReact));

    paint += wallCol   * (wallReact * (0.90 + 0.85 * gridLine) * glowPow);
    paint += float3(1.0, 0.88, 0.50) * (pow(wallReact, 2.6) * glowPow * 0.75); // hot molten core at close wall tap
    paint += tandemCol * ((carReact + carArc * 1.45) * glowPow);

    float3 rimCol = lerp(lerp(wallCol, tandemCol, 0.5), wallCol, saturate(wallReact * 1.2 - carReact));
    if (carReact > wallReact) rimCol = tandemCol;
    paint += rimCol * (fres * (0.14 + 1.35 * totalReact) * glowPow * 0.45);

    // Preserve painted vinyls with glowing rim around decals
    float4 liv = Livery(i);
    float livAlpha = saturate(liv.a * g_fxParam[2].w);
    float3 litLivery = liv.rgb * (0.68 + 0.55 * fres + 0.35 * sunSpec)
                     + wallCol * (wallReact * 0.45 * glowPow)
                     + tandemCol * (carReact * 0.45 * glowPow);
    float3 finalCol = lerp(paint, litLivery, livAlpha);

    return float4(finalCol, 1.0);
}
