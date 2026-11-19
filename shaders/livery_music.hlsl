// Music Reactive Livery v2: Obsidian Midnight Chrome + 3D Body-Wrapped LED Equalizer + Bass Shockwaves
// @mode replace
// @color basePaint       p0.xyz = 0.03 0.035 0.055 : Deep midnight obsidian paint
// @param clearcoat       p0.w 0.0..1.5 = 0.95      : Wet clearcoat & metallic reflection
// @color bassColor       p1.xyz = 1.00 0.08 0.42   : Bass & lower EQ neon (electric crimson/magenta)
// @param bassPulse       p1.w 0.0..3.0 = 1.45      : Bass shockwave & sub glow
// @color midColor        p2.xyz = 0.00 0.88 1.00   : Mid/high EQ neon (hyper cyan)
// @param eqGain          p2.w 0.0..3.0 = 1.50      : Equalizer bar height & brightness
// @color peakColor       p3.xyz = 1.00 0.85 0.35   : Peak LED cap & beat rim flash (neon gold)
// @param beatRim         p3.w 0.0..3.0 = 1.60      : Beat Fresnel edge flash
// @param eqColumns       p4.x 8.0..48.0 = 26.0     : Number of equalizer columns along body
// @param waveSpeed       p4.y 0.2..4.0 = 1.35      : Shockwave & energy flow speed
// @param microFlake      p4.z 0.0..2.0 = 0.65      : Fine metallic pearl shimmer on treble
// @param keepLivery      p4.w 0.0..1.0 = 0.88      : Keep player livery decals & vinyls
#include "carfx.hlsli"

float3 SoftTonemap(float3 c) {
    // Saturation-preserving filmic shoulder: keeps neons vivid without clipping to chalky white
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    float mappedLuma = luma / (1.0 + luma * 0.42);
    float scale = (luma > 1e-4) ? (mappedLuma / luma) : 1.0;
    return c * scale;
}

float Hash31(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);
    float ndotv = saturate(dot(n, v));
    float fres = pow(1.0 - ndotv, 3.2);
    float sharpFres = pow(1.0 - ndotv, 4.5);

    // Parameters
    float3 colBase    = g_fxParam[0].xyz;
    float  clearcoat  = g_fxParam[0].w;
    float3 colBass    = g_fxParam[1].xyz;
    float  bassPulse  = g_fxParam[1].w;
    float3 colMid     = g_fxParam[2].xyz;
    float  eqGain     = g_fxParam[2].w;
    float3 colPeak    = g_fxParam[3].xyz;
    float  beatRim    = g_fxParam[3].w;
    float  eqCols     = max(8.0, floor(g_fxParam[4].x + 0.5));
    float  waveSpeed  = g_fxParam[4].y;
    float  microFlake = g_fxParam[4].z;
    float  keepLivery = g_fxParam[4].w;

    float3 lp = CarLocalPos(i);
    float3 ln = CarLocalNormal(i);

    // 1. Sculpted Midnight Obsidian / Liquid Clearcoat Base (high contrast, real automotive curves)
    float3 keyDir  = normalize(float3(0.45, 0.82, -0.35));
    float3 fillDir = normalize(float3(-0.65, 0.35, 0.65));
    float  keyDiff = saturate(dot(n, keyDir) * 0.5 + 0.5);
    float  skyGrad = saturate(r.y * 0.5 + 0.5);

    float3 envDark   = float3(0.015, 0.018, 0.028);
    float3 envSky    = lerp(float3(0.05, 0.08, 0.14), float3(0.32, 0.44, 0.62), pow(skyGrad, 2.2));
    float  horizonBand = exp(-abs(r.y - 0.06) * 14.0);
    float3 envRefl   = lerp(envDark, envSky, smoothstep(-0.15, 0.25, r.y)) + float3(0.18, 0.24, 0.36) * horizonBand;

    float  broadSpec = pow(saturate(dot(r, keyDir)), 18.0) * 0.35;
    float  coatSpec  = pow(saturate(dot(r, keyDir)), 96.0) * 1.45;
    float  fillSpec  = pow(saturate(dot(r, fillDir)), 28.0) * 0.22;

    float3 bodyPaint = colBase * (0.35 + 0.95 * keyDiff);
    bodyPaint += envRefl * (0.14 + 0.86 * fres) * clearcoat;
    bodyPaint += float3(0.85, 0.92, 1.0) * (broadSpec * 0.4 + coatSpec * (0.35 + 0.65 * fres)) * clearcoat;
    bodyPaint += lerp(colMid, colBass, 0.5) * fillSpec * 0.25;

    // 2. Audio Reactivity
    bool  audioOn = AudioEnabled();
    float aLevel  = AudioLevel();
    float aBass   = AudioBass();
    float aMid    = AudioMid();
    float aTreble = AudioTreble();
    float aBeat   = AudioBeat();

    float3 emission = float3(0.0, 0.0, 0.0);

    if (audioOn && (aLevel > 0.001 || aBeat > 0.001)) {
        float t = g_fxTime.x;

        // Surface masks: vertical body flanks/bumpers vs horizontal roof/hood/trunk/wing
        float topMask  = smoothstep(0.45, 0.80, abs(ln.y));
        float sideMask = 1.0 - topMask;

        // A. 3D Body-Wrapped Symmetrical Segmented LED Spectrum Equalizer (sides + rear/front fascia)
        // Use clean unwrapped horizontal coordinate: Z along flanks, X across bumpers
        float isFrontRear = smoothstep(0.45, 0.78, abs(ln.z));
        float horizPos = lerp(lp.z / 2.35, lp.x / 1.00, isFrontRear); // ~ -1..+1
        float eqU = saturate(horizPos * 0.5 + 0.5);

        // Discrete LED columns with crisp dark bezel gaps
        float colScaled = eqU * eqCols;
        float colIdx = floor(colScaled);
        float colFrac = frac(colScaled);
        float colGap = smoothstep(0.06, 0.14, colFrac) * (1.0 - smoothstep(0.86, 0.94, colFrac));

        // Symmetrical center-outward frequency mapping: bass anchors center, mids/highs radiate symmetrically outward!
        float colCenter = (colIdx + 0.5) / eqCols;
        float symU = saturate(abs(colCenter * 2.0 - 1.0));
        float curvedU = pow(symU, 1.35);
        float specVal = AudioSpectrum(curvedU);
        float specNeighbor = 0.5 * (AudioSpectrum(saturate(curvedU - 0.035)) + AudioSpectrum(saturate(curvedU + 0.035)));
        float barHeight = saturate((specVal * 0.72 + specNeighbor * 0.28) * eqGain * 1.25 + aBass * (1.0 - symU) * 0.22);

        // Vertical height along side panels (0.14m rocker panel -> 0.96m beltline)
        float barY = saturate((lp.y - 0.12) / 0.82);

        // Horizontal LED brick segments (14 crisp segments per bar)
        const float kSegs = 14.0;
        float segScaled = barY * kSegs;
        float segIdx = floor(segScaled);
        float segFrac = frac(segScaled);
        float segGap = smoothstep(0.10, 0.20, segFrac) * (1.0 - smoothstep(0.80, 0.90, segFrac));
        float segCenterY = (segIdx + 0.5) / kSegs;

        // Lit segments + glowing peak cap LED right at top of bar
        float litMask = step(segCenterY, barHeight) * colGap * segGap;
        float peakDist = abs(segCenterY - barHeight);
        float peakCap = smoothstep(0.09, 0.01, peakDist) * colGap * segGap * step(0.06, barHeight);

        // Color ramp: deep bass magenta at bottom -> hyper cyan in middle -> hot gold/white at top
        float3 eqCol = lerp(colBass, colMid, smoothstep(0.12, 0.72, segCenterY));
        eqCol = lerp(eqCol, colPeak, smoothstep(0.72, 0.96, segCenterY));

        // Subtle dark unlit LED matrix backing so the tech grid is faintly visible on beats
        float unlitGrid = colGap * segGap * 0.035 * (0.4 + 0.6 * aLevel);
        float3 eqLayer = eqCol * (litMask * (1.35 + 0.65 * aBeat)) + colPeak * (peakCap * 2.1) + colMid * unlitGrid;

        // Subtle soft bleed aura behind active bars (keeps contrast dark outside bars!)
        float auraMask = smoothstep(barHeight + 0.14, max(0.0, barHeight - 0.08), barY) * step(0.05, barHeight);
        eqLayer += lerp(colBass, colMid, barY) * (auraMask * 0.14 * aLevel);

        emission += eqLayer * sideMask;

        // B. Top Surfaces (Hood, Roof, Trunk, Spoiler Blade): Sonic Bass Shockwaves + Flowing Energy Veins
        float2 topCoord = float2(lp.x * 1.15, lp.z * 0.52);
        float radial = length(topCoord);

        // Crisp concentric shockwave rings expanding outward on bass
        float ringPhase = radial * 9.0 - t * waveSpeed * 4.5;
        float ringCore = pow(saturate(sin(ringPhase) * 0.5 + 0.5), 10.0);
        float ringWide = pow(saturate(sin(ringPhase) * 0.5 + 0.5), 3.0);

        // Subtle longitudinal cyber-flow noise along hood/roof
        float flow = Fbm3(float3(lp.x * 2.2, lp.y * 1.5, lp.z * 1.4 - t * waveSpeed * 1.1));
        float vein = smoothstep(0.58, 0.76, flow) * (0.35 + 0.65 * aMid);

        float3 topRings = colBass * (ringCore * 1.85 + ringWide * 0.28) * (aBass * bassPulse);
        float3 topVeins = colMid * (vein * (0.65 + 0.9 * aMid) * eqGain * 0.55);
        // Add subtle center-line spectrum stripe along hood/roof/trunk spine
        float spineBand = exp(-abs(lp.x) * 9.0);
        float spineSpec = AudioSpectrum(saturate(lp.z * 0.23 + 0.5));
        float3 spineGlow = lerp(colBass, colMid, saturate(lp.z * 0.25 + 0.5)) * (spineBand * spineSpec * 1.15 * eqGain);

        emission += (topRings + topVeins + spineGlow) * topMask;

        // C. Global Expanding Bass Pulse Rim (subtle wave wrapping entire body + spoiler)
        float sweepWave = pow(saturate(sin(lp.z * 3.8 - t * waveSpeed * 5.2) * 0.5 + 0.5), 12.0);
        emission += colBass * (sweepWave * aBass * bassPulse * 0.35);

        // D. Razor-Sharp Beat Rim & Fender Contour Highlight
        float rimEnergy = sharpFres * (0.28 * aLevel + 1.15 * aBeat) * beatRim;
        float3 rimColor = lerp(colMid, colPeak, saturate(aBeat * 1.2));
        emission += rimColor * rimEnergy;

        // E. Ultra-Fine Metallic Candy Pearl Flake on Treble (3D micro-sparkle, ZERO big squares!)
        if (microFlake > 0.01 && aTreble > 0.01) {
            float3 flakeCell = floor(lp * 280.0);
            float h1 = Hash31(flakeCell);
            float h2 = Hash31(flakeCell + 17.31);
            // View-dependent glint so flakes twinkle naturally as the car drifts
            float glintAngle = sin((h2 * 6.28318) + dot(v, float3(11.0, 17.0, 13.0)) + t * 4.0) * 0.5 + 0.5;
            float sparkle = step(0.965, h1) * pow(glintAngle, 6.0);
            float flakeStrength = sparkle * (0.25 + 0.75 * fres + coatSpec) * aTreble * microFlake * 2.2;
            emission += lerp(colMid, colPeak, h2) * flakeStrength;
        }
    } else {
        // Sleek idle showcase look when music is silent or capture is off
        float idlePulse = 0.5 + 0.5 * sin(g_fxTime.x * 1.8 - lp.z * 1.6);
        emission += lerp(colBass, colMid, saturate(lp.z * 0.25 + 0.5)) * (sharpFres * (0.22 + 0.14 * idlePulse));
    }

    float3 finalBody = SoftTonemap(bodyPaint + emission);

    // 3. Preserve Player Livery Vinyls & Decals cleanly
    float4 livery = Livery(i);
    float livAlpha = saturate(livery.a * keepLivery);
    float3 litLivery = livery.rgb * (0.78 + 0.32 * keyDiff + 0.25 * fres) + SoftTonemap(emission) * 0.22;
    float3 finalColor = lerp(finalBody, litLivery, livAlpha);

    return float4(finalColor, 1.0);
}
