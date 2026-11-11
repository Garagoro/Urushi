// Cyber Circuit Pulse: Animated Honeycomb & Neon Energy Shockwave Reacting to Drift, RPM & Throttle.
// Perfect for drift edit showcases: deep glossy obsidian clearcoat + glowing hex traces + travelling plasma wave.
// @param speed      p0.x 0.1..5.0 = 1.25 : energy sweep & pulse animation speed
// @param hexScale   p0.y 8.0..120 = 42.0 : honeycomb & circuit cell density
// @param pulseWidth p0.z 0.05..0.6 = 0.22 : shockwave band thickness across car body
// @param driftBoost p0.w 0.0..3.0 = 1.6  : drift surge transition & brightness boost
// @color primaryCol p1.xyz = 0.05 0.90 1.0 : primary cyber neon colour
// @param intensity  p1.w 0.2..6.0 = 2.6  : HDR emission multiplier
// @color driftCol   p2.xyz = 1.0 0.16 0.56 : secondary / drift overdrive colour
// @param keepLivery p2.w 0.0..1.0 = 0.85 : keep painted vinyl decals + glow outlines
// @color basePaint  p3.xyz = 0.04 0.05 0.09 : dark metallic clearcoat body base
// @param fresnelRim p3.w 0.0..4.0 = 1.75 : grazing Fresnel silhouette glow
// @mode replace
#include "carfx.hlsli"

float HexEdgeDist(float2 p) {
    p = abs(p);
    return max(dot(p, float2(0.5, 0.8660254)), p.x);
}

// Explicit floor modulo so UV tiling is continuous across negative/positive coords
float4 HexGrid(float2 uv) {
    float2 r = float2(1.0, 1.7320508);
    float2 h = r * 0.5;
    float2 a = uv - r * floor(uv / r) - h;
    float2 b = (uv - h) - r * floor((uv - h) / r) - h;
    float2 gv = (dot(a, a) < dot(b, b)) ? a : b;
    float2 id = uv - gv;
    float edgeDist = 0.5 - HexEdgeDist(gv);
    return float4(gv.x, gv.y, edgeDist, Hash13(float3(id, 1.0)));
}

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);
    float ndotv = saturate(dot(n, v));
    float fres = pow(1.0 - ndotv, 2.6);

    float speed = g_fxParam[0].x;
    float t = g_fxTime.x * speed;

    float driftMix = saturate(CarDriftNorm() * g_fxParam[0].w);
    float revPulse = 0.12 * saturate(CarThrottle()) * (0.5 + 0.5 * sin(g_fxTime.x * 14.0));
    float brakeHot = saturate(CarBrake() - 0.15);

    // 1. Dark satin obsidian metallic clearcoat lighting
    float horizon = smoothstep(-0.15, 0.25, r.y);
    float3 skyEnv = lerp(float3(0.06, 0.08, 0.14), float3(0.55, 0.72, 0.95), pow(saturate(r.y * 0.5 + 0.5), 1.4));
    float3 gndEnv = lerp(float3(0.015, 0.018, 0.025), float3(0.09, 0.10, 0.13), saturate(-r.y));
    float3 sunDir = normalize(float3(0.45, 0.82, -0.35));
    float sunSpec = pow(saturate(dot(r, sunDir)), 52.0) * 1.45;
    float sheenSpec = pow(saturate(dot(r, normalize(float3(-0.6, 0.5, 0.5)))), 18.0) * 0.45;
    float3 bodyMetal = lerp(gndEnv, skyEnv, horizon) * (g_fxParam[3].xyz * 1.65) + g_fxParam[3].xyz * 0.45;
    bodyMetal += float3(0.85, 0.92, 1.0) * (sunSpec * (0.35 + 0.65 * fres) + sheenSpec * fres);

    // 2. Crisp procedural honeycomb micro-circuit locked to car UV
    float2 cellUv = i.uv * g_fxParam[0].y;
    float4 hx = HexGrid(cellUv);
    float wireThin = 1.0 - smoothstep(0.018, 0.052, hx.z);
    float wireCore = 1.0 - smoothstep(0.004, 0.022, hx.z);

    // 3. Sweeping shockwave travelling across body panels + counter wave
    float flowCoord = i.uv.x * 0.75 + i.uv.y * 0.95 + n.y * 0.12;
    float dWave = abs(frac(flowCoord - t * 0.42) - 0.5) * 2.0;
    float pw = max(g_fxParam[0].z, 0.04);
    float shockWave = pow(saturate(1.0 - dWave / pw), 3.0);

    float dWave2 = abs(frac(flowCoord * 1.7 + t * 0.25 + 0.35) - 0.5) * 2.0;
    float subWave = pow(saturate(1.0 - dWave2 / (pw * 1.3)), 4.0) * 0.55;

    // 4. Individual cell ignition + travelling plasma sparkles
    float cellFlicker = 0.5 + 0.5 * sin(t * 3.4 + hx.w * 37.0 + shockWave * 3.14);
    float cellActive = smoothstep(0.45, 0.98, cellFlicker) * (0.28 + 0.95 * shockWave + 0.6 * driftMix);

    // 5. Dynamic cyber color palette (primary cyan <-> drift magenta/amber)
    float3 colPrimary = g_fxParam[1].xyz;
    float3 colDrift   = g_fxParam[2].xyz;
    float colorBlend  = saturate(shockWave * 0.45 + driftMix * 0.85 + subWave * 0.35);
    float3 neonCol    = lerp(colPrimary, colDrift, colorBlend);
    neonCol = lerp(neonCol, float3(1.0, 0.28, 0.14), brakeHot * fres * 0.55);

    // Combine glow lines, glowing cells, hot white core on shockfront & silhouette Fresnel rim
    float neonMask = wireThin * (0.22 + 1.45 * shockWave + 0.55 * subWave + 0.65 * driftMix + revPulse)
                   + cellActive * 0.22
                   + wireCore * shockWave * 1.1;

    float3 energy = neonCol * (neonMask * g_fxParam[1].w);
    energy += colPrimary * (wireCore * shockWave * 0.55 * g_fxParam[1].w); // hot core crest
    energy += lerp(colPrimary, colDrift, saturate(driftMix + shockWave * 0.3)) * (fres * g_fxParam[3].w * (0.35 + 0.65 * shockWave + 0.5 * driftMix));

    float3 paint = bodyMetal + energy;

    // 6. Optional painted vinyl preservation + neon contour outline around decals
    float4 liv = Livery(i);
    float livAlpha = saturate(liv.a * g_fxParam[2].w);
    float livEdge = saturate(fwidth(liv.a) * 5.0) * g_fxParam[2].w;
    paint += neonCol * (livEdge * g_fxParam[1].w * 0.9);
    float3 litLivery = liv.rgb * (0.65 + 0.6 * fres + 0.4 * sunSpec) + neonCol * (0.35 * shockWave);
    float3 finalCol = lerp(paint, litLivery, livAlpha);

    return float4(finalCol, 1.0);
}
