// Tandem & 65° Angle Reactive: Dual-layer shader driven by opponent car proximity + real slip angle.
//   - Body finish morphs with an exponential slip-angle curve (ignoring tiny ~1-2° wobble)
//   - Color sweeps progressively from the front bumper toward the rear bumper as angle grows up to 65°
//   - Directional slip-stream highlights sweep along the drifting flank
//   - Nearby opponent car / 3D ghost ignites electric plasma along the facing panels (OBB 3D SDF)
// @param carRange    p0.y 0.5..10.0 = 4.2  : tandem opponent detection radius (meters)
// @param maxAngleDeg p0.x 25.0..90.0 = 65.0 : slip angle for 100% peak color/aura shift (deg)
// @param expCurve    p1.z 1.0..8.0  = 3.6  : exponential steepness of angle response curve
// @param flowSpeed   p5.x 0.5..30.0 = 8.0  : smooth transition speed for proximity & angle
// @param pulseSpeed  p0.z 0.0..8.0  = 2.4  : energy pulse speed (independent of car speed)
// @param glowPower   p0.w 0.2..6.0  = 2.8  : plasma glow & rim intensity
// @color angleCol0   p3.xyz = 0.04 0.11 0.26 : base paint tint at 0° (straight)
// @color angleCol65  p4.xyz = 0.95 0.06 0.42 : peak paint tint at 65° drift angle
// @color tandemCol   p2.xyz = 0.02 0.92 1.00 : opponent proximity plasma colour
// @param angleBlend  p1.x 0.0..1.0  = 0.92 : strength of 0°->65° body transformation
// @param gridScale   p1.y 0.5..8.0  = 3.0  : aerodynamic stream & tandem energy frequency
// @param keepLivery  p2.w 0.0..1.0  = 0.85 : preserve original decals / vinyls
// @param fresnelPow  p4.w 0.5..6.0  = 2.6  : clearcoat rim sharpness
// @mode replace
#include "carfx.hlsli"

float ExpNorm(float x, float k) {
    float ks = max(k, 0.25);
    return saturate((exp(ks * saturate(x)) - 1.0) / max(exp(ks) - 1.0, 1e-4));
}

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);
    float fres = Fresnel(i, g_fxParam[4].w);

    float maxAngleDeg = max(g_fxParam[0].x, 5.0);
    float carRange    = max(g_fxParam[0].y, 0.25);
    float pulseSpeed  = g_fxParam[0].z;
    float glowPow     = g_fxParam[0].w;
    float angleBlend  = g_fxParam[1].x;
    float gridScale   = g_fxParam[1].y;
    float expSteep    = max(g_fxParam[1].z, 0.5);
    float t           = g_fxTime.x * pulseSpeed;

    float3 lp = CarLocalPos(i);
    float hw = max(g_fxCarPos.w, 0.85);
    float hl = max(g_fxCarRight.w, 1.80);

    // 1. Exponential slip angle response + progressive front-to-rear fill peaking at maxAngleDeg (65°)
    float slipAbsDeg = CarDriftAngleDeg();
    float slipSigned = CarDriftSignedDeg();
    // Ignore tiny ~2° straight-line steering/camber jitter; accelerate exponentially toward 65°
    float angLinear  = saturate((slipAbsDeg - 2.0) / max(maxAngleDeg - 2.0, 5.0));
    float fillProg   = ExpNorm(angLinear, expSteep * 0.88); // front-to-rear spread reach (0..1)
    float angExp     = ExpNorm(angLinear, expSteep * 1.15); // intensity exponential curve (0..1)
    float slipDir    = clamp(slipSigned / maxAngleDeg, -1.0, 1.0);

    // Normalized coordinate strictly along car length: 0.0 at front bumper tip -> 1.0 at rear bumper tip
    float frontToRear = saturate((hl * 1.03 - lp.z) / (2.06 * hl));
    float edgeSoft    = 0.13;
    float wavePos     = fillProg * (1.0 + 1.6 * edgeSoft) - edgeSoft * 0.5;
    float waveReach   = 1.0 - smoothstep(wavePos - edgeSoft * 0.5, wavePos + edgeSoft * 0.5, frontToRear);
    float activeGate  = smoothstep(0.0, 0.025, fillProg);

    // Pure exponential front-to-rear fill mask (strictly 0.0 at 0-2°, reaches rear cleanly at 65°)
    float fillMask    = saturate(waveReach * activeGate);
    float shiftMask   = fillMask * angleBlend;
    float waveFront   = 4.0 * fillMask * (1.0 - fillMask) * smoothstep(0.0, 0.08, fillProg) * (1.0 - smoothstep(0.92, 1.0, fillProg));

    float3 col0  = g_fxParam[3].xyz;
    float3 col65 = g_fxParam[4].xyz;
    float3 baseTint = lerp(col0, col65, shiftMask);

    float horizon = smoothstep(-0.16, 0.24, r.y);
    float3 skyEnv = lerp(float3(0.05, 0.08, 0.14), float3(0.72, 0.84, 1.00), pow(saturate(r.y * 0.5 + 0.5), 1.35));
    float3 gndEnv = lerp(float3(0.015, 0.018, 0.025), float3(0.09, 0.10, 0.13), saturate(-r.y));
    float sunSpec = pow(saturate(dot(r, normalize(float3(0.45, 0.82, -0.35)))), 64.0) * 1.75;

    float localIntensity = fillMask * (0.30 + 0.70 * angExp) * angleBlend;
    float3 paint = lerp(gndEnv, skyEnv, horizon) * (baseTint * 2.15 + 0.03) + baseTint * (0.58 + 0.48 * localIntensity);
    paint += float3(0.94, 0.97, 1.0) * (sunSpec * (0.35 + 0.65 * fres));

    // Dynamic slip-angle flow streaks & silhouette aura strictly inside the filled region + bright leading front-to-rear wave
    float flowCoord = lp.z * gridScale + lp.x * slipDir * gridScale * 1.6;
    float slipLines = smoothstep(0.84, 0.99, abs(frac(flowCoord - t * 0.45) - 0.5) * 2.0);
    float localAura = fillMask * pow(angExp, 0.85) * angleBlend;

    paint += col65 * ((fres * (0.35 + 1.25 * localAura) + slipLines * (0.28 + 0.45 * fres)) * localAura * glowPow * 0.72);
    paint += lerp(col65, float3(1.0, 0.88, 0.52), 0.45) * (waveFront * (0.55 + 0.95 * fres + 0.55 * slipLines) * angleBlend * glowPow * 0.68);
    // Warm golden highlight flash on crisp silhouette edge once approaching ~65° peak
    paint += float3(1.0, 0.85, 0.48) * (fillMask * pow(angExp, 2.2) * fres * angleBlend * glowPow * 0.55);

    // 2. Nearby opponent / 3D ghost proximity reaction (OBB 3D distance per pixel)
    float dCarPx = CarPixelToNearCarDist(i);
    float carField = 0.0;
    float carCore  = 0.0;
    if (dCarPx < carRange && HasCarSpatial()) {
        float cNorm = saturate(1.0 - dCarPx / carRange);
        float heat = pow(cNorm, 1.6);

        // Localized electric waves + crisp induction lines facing the other car
        float cWave = pow(0.5 + 0.5 * sin(dCarPx * (6.2 * gridScale * 0.65) - t * 2.8), 2.8);
        float2 gridUV = abs(frac(float2(lp.z, lp.x + lp.y) * gridScale) - 0.5) * 2.0;
        float gridLine = smoothstep(0.87, 0.99, max(gridUV.x, gridUV.y));

        carField = saturate(heat * (0.72 + 0.55 * cWave + 0.45 * gridLine));
        carCore  = pow(saturate(1.0 - dCarPx / max(carRange * 0.34, 0.55)), 1.75);
    }

    float3 tandemCol = g_fxParam[2].xyz;
    paint += tandemCol * (carField * glowPow * 1.05);
    paint += float3(0.82, 0.98, 1.0) * (carCore * glowPow * 1.15);
    paint += tandemCol * (fres * carField * glowPow * 0.70);

    // 3. Preserve decals / vinyls cleanly
    float4 liv = Livery(i);
    float livAlpha = saturate(liv.a * g_fxParam[2].w);
    float3 litLivery = liv.rgb * (0.70 + 0.52 * fres + 0.32 * sunSpec)
                     + col65 * (localAura * 0.35 * glowPow)
                     + lerp(col65, float3(1.0, 0.88, 0.52), 0.45) * (waveFront * angleBlend * glowPow * 0.25)
                     + tandemCol * (carField * 0.45 * glowPow)
                     + float3(0.82, 0.98, 1.0) * (carCore * 0.45 * glowPow);
    float3 finalCol = lerp(paint, litLivery, livAlpha);

    return float4(finalCol, 1.0);
}
