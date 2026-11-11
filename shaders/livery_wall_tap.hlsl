// Wall-Tap Reactor: Dedicated multi-point wall & barrier proximity reactive shader.
// Dark forged-carbon obsidian body that ignites with crisp plasma shockwaves & hex armor
// on ANY approaching front bumper, rear bumper, left/right door (between wheels), or corners simultaneously.
// @param wallRange  p0.x 0.5..10.0 = 3.8  : wall detection radius (meters)
// @param flowSpeed  p5.x 0.5..30.0 = 6.0  : smooth transition & fade speed (lower = softer flow)
// @param pulseSpeed p0.z 0.0..8.0  = 2.6  : shockwave pulse speed (independent of car speed)
// @param glowPower  p0.w 0.2..6.0  = 3.2  : plasma flare HDR intensity
// @color wallCol    p1.xyz = 1.0 0.28 0.02 : primary wall-tap neon / lava colour
// @param weaveStr   p1.w 0.0..1.0  = 0.35 : subtle carbon-weave surface detail
// @color coreCol    p2.xyz = 1.0 0.86 0.42 : white-hot close-contact plasma core colour
// @param keepLivery p2.w 0.0..1.0  = 0.85 : preserve original decals / vinyls
// @color baseCol    p3.xyz = 0.045 0.055 0.08 : base obsidian metallic paint
// @param hexScale   p3.w 1.0..14.0 = 6.5  : metric energy armor grid density
// @param rimBoost   p4.x 0.0..2.5  = 0.90 : Fresnel clearcoat & rim energy boost
// @param fresnelPow p4.w 0.5..6.0  = 2.8  : clearcoat rim light sharpness
// @mode replace
#include "carfx.hlsli"

float HexGridMask(float2 p) {
    // Smooth architectural tech-hex lattice in car-local meters
    float2 r = float2(1.0, 1.7320508);
    float2 h = r * 0.5;
    float2 a = fmod(abs(p), r) - h;
    float2 b = fmod(abs(p + h), r) - h;
    float2 gv = dot(a, a) < dot(b, b) ? a : b;
    float edge = 0.5 - max(abs(gv.x), dot(abs(gv), float2(0.5, 0.8660254)));
    return 1.0 - smoothstep(0.025, 0.095, edge);
}

struct WallSpotRes {
    float field; // outer glowing aura + traveling waves + hex reveal
    float core;  // razor-hot contact crest right near the wall
};

WallSpotRes EvalWallSpot(PSIn i, float4 wallHit, float maxRange, float timeVal, float hexScale) {
    WallSpotRes res;
    res.field = 0.0;
    res.core  = 0.0;
    if (wallHit.w >= maxRange || !HasCarSpatial()) return res;

    float closeNorm = saturate(1.0 - wallHit.w / maxRange);
    float heat = pow(closeNorm, 1.55);

    float dPx = CarPixelToWallHitDist(i, wallHit);
    float dSurf = max(dPx - wallHit.w, 0.0);

    // Focused footprint per bumper / door / corner that expands slightly as gap shrinks
    float sigma = lerp(0.88, 0.56, heat);
    float radial = exp(-pow(dSurf / sigma, 2.0) * 2.1);

    // Constant time frequency => completely invariant to car speed
    float phase = dSurf * 5.8 - timeVal * 2.6;
    float wave = pow(0.5 + 0.5 * sin(phase), 3.2) * exp(-dSurf * 1.45);

    // Shockwave crest & intense wall-scrape flare under ~0.85m
    float contactBand = pow(saturate(1.0 - wallHit.w / 0.85), 1.45) * exp(-dSurf * dSurf * 2.6);

    res.field = saturate(heat * (radial * 0.82 + wave * 0.65));
    res.core  = saturate(contactBand * (0.85 + 0.45 * pow(0.5 + 0.5 * sin(dSurf * 10.0 - timeVal * 3.4), 2.0)));
    return res;
}

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);
    float fres = Fresnel(i, g_fxParam[4].w);

    float wallRange = max(g_fxParam[0].x, 0.25);
    float speed     = g_fxParam[0].z;
    float glowPow   = g_fxParam[0].w;
    float weaveStr  = g_fxParam[1].w;
    float hexScale  = g_fxParam[3].w;
    float rimBoost  = g_fxParam[4].x;
    float t         = g_fxTime.x * speed;

    float3 lp = CarLocalPos(i);
    float3 ln = CarLocalNormal(i);

    // 1. Deep Obsidian / Forged Carbon Clearcoat Base
    float weave = 0.0;
    if (HasCarSpatial() && weaveStr > 0.001) {
        float2 cUv = (abs(ln.y) > 0.55) ? lp.xz : ((abs(ln.x) > 0.55) ? lp.zy : lp.xy);
        float cPat = sin(cUv.x * 95.0 + sin(cUv.y * 95.0)) * cos(cUv.y * 95.0);
        weave = (cPat * 0.5 + 0.5) * weaveStr;
    }

    float3 baseTint = g_fxParam[3].xyz;
    float horizon = smoothstep(-0.18, 0.22, r.y);
    float3 skyEnv = lerp(float3(0.05, 0.07, 0.11), float3(0.68, 0.80, 0.98), pow(saturate(r.y * 0.5 + 0.5), 1.4));
    float3 gndEnv = lerp(float3(0.015, 0.018, 0.022), float3(0.08, 0.09, 0.12), saturate(-r.y));
    float sunSpec = pow(saturate(dot(r, normalize(float3(0.42, 0.84, -0.34)))), 72.0) * 1.9;

    float3 paint = lerp(gndEnv, skyEnv, horizon) * (baseTint * (2.2 + 0.7 * weave) + 0.025)
                 + baseTint * (0.48 + 0.30 * weave);
    paint += float3(0.92, 0.96, 1.0) * (sunSpec * (0.35 + 0.65 * fres));

    // 2. Evaluate all 8 wall proximity zones independently & simultaneously
    float hw = max(g_fxCarPos.w, 0.85);
    float hl = max(g_fxCarRight.w, 1.80);
    float lxN = lp.x / hw;
    float lzN = lp.z / hl;

    WallSpotRes wF  = EvalWallSpot(i, g_fxWallFrontHit,     wallRange, t, hexScale);
    WallSpotRes wR  = EvalWallSpot(i, g_fxWallRearHit,      wallRange, t, hexScale);
    WallSpotRes wDL = EvalWallSpot(i, g_fxWallDoorLeftHit,  wallRange, t, hexScale);
    WallSpotRes wDR = EvalWallSpot(i, g_fxWallDoorRightHit, wallRange, t, hexScale);
    WallSpotRes wFL = EvalWallSpot(i, g_fxWallFrontLeftHit, wallRange, t, hexScale);
    WallSpotRes wFR = EvalWallSpot(i, g_fxWallFrontRightHit,wallRange, t, hexScale);
    WallSpotRes wRL = EvalWallSpot(i, g_fxWallRearLeftHit,  wallRange, t, hexScale);
    WallSpotRes wRR = EvalWallSpot(i, g_fxWallRearRightHit, wallRange, t, hexScale);

    float mF  = smoothstep(-0.05, 0.45,  lzN) * lerp(0.28, 1.0, smoothstep(-0.20, 0.35,  ln.z));
    float mR  = smoothstep(-0.05, 0.45, -lzN) * lerp(0.28, 1.0, smoothstep(-0.20, 0.35, -ln.z));
    float doorBelt = 1.0 - smoothstep(0.42, 0.72, abs(lzN));
    float mDL = smoothstep(-0.12, 0.35, -lxN) * lerp(0.18, 1.0, smoothstep(-0.25, 0.30, -ln.x)) * lerp(0.35, 1.0, doorBelt);
    float mDR = smoothstep(-0.12, 0.35,  lxN) * lerp(0.18, 1.0, smoothstep(-0.25, 0.30,  ln.x)) * lerp(0.35, 1.0, doorBelt);
    float mFL = smoothstep(-0.12, 0.35, -lxN) * smoothstep(-0.05, 0.45,  lzN) * lerp(0.20, 1.0, smoothstep(-0.25, 0.30, max(-ln.x,  ln.z)));
    float mFR = smoothstep(-0.12, 0.35,  lxN) * smoothstep(-0.05, 0.45,  lzN) * lerp(0.20, 1.0, smoothstep(-0.25, 0.30, max( ln.x,  ln.z)));
    float mRL = smoothstep(-0.12, 0.35, -lxN) * smoothstep(-0.05, 0.45, -lzN) * lerp(0.20, 1.0, smoothstep(-0.25, 0.30, max(-ln.x, -ln.z)));
    float mRR = smoothstep(-0.12, 0.35,  lxN) * smoothstep(-0.05, 0.45, -lzN) * lerp(0.20, 1.0, smoothstep(-0.25, 0.30, max( ln.x, -ln.z)));

    float aF = wF.field * mF,   cF = wF.core * mF;
    float aR = wR.field * mR,   cR = wR.core * mR;
    float aDL= wDL.field * mDL, cDL= wDL.core * mDL;
    float aDR= wDR.field * mDR, cDR= wDR.core * mDR;
    float aFL= wFL.field * mFL, cFL= wFL.core * mFL;
    float aFR= wFR.field * mFR, cFR= wFR.core * mFR;
    float aRL= wRL.field * mRL, cRL= wRL.core * mRL;
    float aRR= wRR.field * mRR, cRR= wRR.core * mRR;

    // Simultaneous screen-blend across all 8 perimeter zones (Front + Door + Rear all stay active together)
    float wallField = saturate(
        1.0 - (1.0 - aF) * (1.0 - aR) * (1.0 - aDL) * (1.0 - aDR)
            * (1.0 - aFL) * (1.0 - aFR) * (1.0 - aRL) * (1.0 - aRR)
    );
    float wallCore = saturate(
        1.0 - (1.0 - cF) * (1.0 - cR) * (1.0 - cDL) * (1.0 - cDR)
            * (1.0 - cFL) * (1.0 - cFR) * (1.0 - cRL) * (1.0 - cRR)
    );

    // Hex energy armor only appears inside activated proximity field (zero clutter away from walls)
    float2 hexCoord = float2(lp.z, lp.x * 0.65 + lp.y * 1.15) * hexScale;
    float hex = HexGridMask(hexCoord);
    float edgeMask = smoothstep(0.08, 0.65, wallField);

    float3 wallCol = g_fxParam[1].xyz;
    float3 coreCol = g_fxParam[2].xyz;

    paint += wallCol * (wallField * (0.65 + 1.05 * hex * edgeMask) * glowPow);
    paint += coreCol * (pow(wallCore, 1.25) * glowPow * 1.15);
    paint += lerp(wallCol, coreCol, saturate(wallCore * 1.4)) * (fres * (0.04 + rimBoost * wallField) * glowPow * 0.65);

    // 3. Preserve original vinyls/decals cleanly
    float4 liv = Livery(i);
    float livAlpha = saturate(liv.a * g_fxParam[2].w);
    float3 litLivery = liv.rgb * (0.72 + 0.50 * fres + 0.30 * sunSpec)
                     + wallCol * (wallField * 0.45 * glowPow)
                     + coreCol * (wallCore * 0.55 * glowPow);
    float3 finalCol = lerp(paint, litLivery, livAlpha);

    return float4(finalCol, 1.0);
}
