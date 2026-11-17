// Music Reactive Livery: Pulses, waves and flashes to music playing on the PC.
// @mode replace
// @color basePaint       p0.xyz = 0.04 0.04 0.06 : Deep metallic clearcoat base
// @param baseMetallic    p0.w 0.0..1.0 = 0.85    : Paint clearcoat reflection
// @color bassColor       p1.xyz = 0.88 0.12 0.95 : Bass pulse color (magenta / purple)
// @param bassBoost       p1.w 0.0..5.0 = 2.2     : Bass wave intensity & expansion
// @color midColor        p2.xyz = 0.05 0.80 1.00 : Mid frequency neon flow (cyan / blue)
// @param midBoost        p2.w 0.0..5.0 = 1.8     : Mid energy flow strength
// @color beatColor       p3.xyz = 1.00 0.96 0.90 : Beat strobe flash (rim & fresnel)
// @param beatBoost       p3.w 0.0..5.0 = 2.5     : Beat flash intensity
// @param sparkleStrength p4.x 0.0..5.0 = 2.2     : Treble sparkle intensity
// @param waveSpeed       p4.y 0.1..5.0 = 1.6     : Pulse ring propagation speed
// @param waveDensity     p4.z 1.0..25.0 = 7.5    : Pulse ring spatial density
// @param keepLivery      p4.w 0.0..1.0 = 0.90    : Keep player livery decals & vinyls
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);
    float ndotv = saturate(dot(n, v));
    float fres = pow(1.0 - ndotv, 2.5);

    // Read parameter controls
    float3 colBase      = g_fxParam[0].xyz;
    float  metallic     = g_fxParam[0].w;
    float3 colBass      = g_fxParam[1].xyz;
    float  bassBoost    = g_fxParam[1].w;
    float3 colMid       = g_fxParam[2].xyz;
    float  midBoost     = g_fxParam[2].w;
    float3 colBeat      = g_fxParam[3].xyz;
    float  beatBoost    = g_fxParam[3].w;
    float  sparkleBoost = g_fxParam[4].x;
    float  waveSpeed    = g_fxParam[4].y;
    float  waveDensity  = g_fxParam[4].z;
    float  keepLivery   = g_fxParam[4].w;

    // 1. Ambient & Environment reflections (calm base look when audio is quiet or off)
    float horizon = smoothstep(-0.2, 0.3, r.y);
    float3 skyEnv = lerp(float3(0.08, 0.10, 0.16), float3(0.60, 0.75, 0.95), pow(saturate(r.y * 0.5 + 0.5), 1.5));
    float3 gndEnv = lerp(float3(0.02, 0.02, 0.03), float3(0.10, 0.11, 0.14), saturate(-r.y));
    float3 sunDir = normalize(float3(0.5, 0.8, -0.4));
    float sunSpec = pow(saturate(dot(r, sunDir)), 48.0) * 1.5;
    float3 envLight = lerp(gndEnv, skyEnv, horizon);

    float3 bodyPaint = colBase * (envLight * 1.5 + 0.3) + skyEnv * (fres * metallic * 0.8);
    bodyPaint += float3(0.9, 0.95, 1.0) * (sunSpec * (0.2 + 0.8 * fres));

    // Audio reactivity (if audio capture is off or silent, all audio values are 0)
    bool audioOn = AudioEnabled();
    float aLevel   = AudioLevel();
    float aBass    = AudioBass();
    float aMid     = AudioMid();
    float aTreble  = AudioTreble();
    float aBeat    = AudioBeat();

    float3 audioEmission = float3(0.0, 0.0, 0.0);

    if (audioOn && (aLevel > 0.001 || aBeat > 0.001)) {
        // A. Bass pulse radiating from the car center outwards
        float3 lp = CarLocalPos(i);
        float distCenter = length(float2(lp.x * 1.35, lp.z * 0.55));
        float pulseTime = g_fxTime.x * waveSpeed * 4.0;
        float wave = sin(distCenter * waveDensity - pulseTime);
        float ringCrest = pow(saturate(wave * 0.5 + 0.5), 3.5);

        // Core bass energy expanding outwards
        float bassEnergy = (ringCrest * 0.75 + aBass * 0.6) * (aBass * bassBoost);
        float3 bassGlow = colBass * bassEnergy;

        // B. Mid frequency audio spectrum & body contour sweep
        // Map 32 log spectrum bands along the car length (Z axis: -2.2m rear to +2.2m front)
        float normZ = saturate(lp.z * 0.23 + 0.5);
        float specValue = AudioSpectrum(normZ);

        float sideFlow = sin(i.uv.x * 20.0 + g_fxTime.x * 2.0) * 0.5 + 0.5;
        float midEnergy = (specValue * 1.2 + aMid * 0.5 * sideFlow) * midBoost;
        float3 midGlow = colMid * midEnergy;

        // C. Treble sparkle (high-frequency glittering flakes across paint)
        float2 sparkUv = floor(i.uv * 220.0);
        float sparkRand = frac(sin(dot(sparkUv, float2(12.9898, 78.233))) * 43758.5453);
        float sparkGlitter = pow(sparkRand, 22.0) * 12.0 * aTreble * sparkleBoost;
        float3 trebleGlow = float3(1.0, 1.0, 1.0) * sparkGlitter;

        // D. Beat onset pulse flash on rim & Fresnel grazing angles
        float beatStrobe = pow(fres, 1.6) * aBeat * beatBoost;
        float3 beatGlow = colBeat * (beatStrobe + aBeat * 0.15);

        // Combine emission
        audioEmission = bassGlow + midGlow + trebleGlow + beatGlow;
    }

    float3 finalBody = bodyPaint + audioEmission;

    // 2. Preserving Player Liveries (decals, vinyls, stickers)
    float4 livery = Livery(i);
    float livAlpha = saturate(livery.a * keepLivery);
    float3 litLivery = livery.rgb * (0.85 + 0.35 * fres) + audioEmission * 0.25;
    float3 finalColor = lerp(finalBody, litLivery, livAlpha);

    return float4(finalColor, 1.0);
}
