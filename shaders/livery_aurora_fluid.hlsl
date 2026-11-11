// Aurora Liquid Chrome: Glossy Flowing Plasma & Iridescent Silk Waves over Mirror Lacquer.
// Smooth organic ribbons across body curves reacting dynamically to drift & RPM.
// @param flowSpeed  p0.x 0.1..5.0 = 1.1  : liquid plasma ribbon speed
// @param ribbonFreq p0.y 1.0..18.0 = 5.5 : aurora wave curvature & frequency
// @param mirrorMix  p0.z 0.0..1.0 = 0.72 : liquid chrome environment sharpness
// @param driftSurge p0.w 0.0..3.0 = 1.5  : overdrive brightness while sliding
// @color auroraA    p1.xyz = 0.12 0.82 1.0 : aurora wave crest A
// @param brightness p1.w 0.2..5.0 = 1.9  : overall emission & gloss boost
// @color auroraB    p2.xyz = 0.95 0.24 0.78 : aurora wave crest B
// @param keepLivery p2.w 0.0..1.0 = 0.85 : keep painted vinyl decals cleanly visible
// @color auroraC    p3.xyz = 1.0 0.72 0.18 : hot molten gold drift highlight
// @param fresnelPow p3.w 0.5..6.0 = 2.4  : clearcoat rim sharpness
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);
    float ndotv = saturate(dot(n, v));
    float fres = Fresnel(i, g_fxParam[3].w);

    float t = g_fxTime.x * g_fxParam[0].x;
    float freq = g_fxParam[0].y;
    float drift = saturate(CarDriftNorm() * g_fxParam[0].w);

    // Mirror chrome base
    float horizon = smoothstep(-0.14, 0.20, r.y);
    float3 sky = lerp(float3(0.05, 0.06, 0.10), float3(0.75, 0.86, 1.05), pow(saturate(r.y * 0.5 + 0.5), 1.3));
    float3 ground = lerp(float3(0.02, 0.02, 0.03), float3(0.11, 0.12, 0.15), saturate(-r.y));
    float3 specDir = normalize(float3(sin(t * 0.7) * 0.65, 0.72, cos(t * 0.7) * 0.65));
    float spec = pow(saturate(dot(r, specDir)), 56.0) * 1.9;
    float3 chrome = lerp(ground, sky, horizon) * lerp(0.28, 1.0, fres) + spec;

    // Domain-warped aurora fluid ribbons across car body UV + view normal
    float2 p = i.uv * freq + float2(n.x, n.z) * 1.4;
    float warp1 = sin(p.x * 1.3 + t * 1.1) + cos(p.y * 1.7 - t * 0.9);
    float warp2 = cos((p.x + p.y) * 1.1 + warp1 * 0.85 - t * 1.4);
    float ribbon = 0.5 + 0.5 * sin(p.y * 2.1 + warp1 * 1.2 + warp2 * 0.9 + t * 1.5);
    float ridge = pow(saturate(1.0 - abs(ribbon * 2.0 - 1.0)), 2.2);

    float mixAB = 0.5 + 0.5 * sin(warp2 * 1.6 + ndotv * 3.14 + t * 0.7);
    float3 aurCol = lerp(g_fxParam[1].xyz, g_fxParam[2].xyz, mixAB);
    aurCol = lerp(aurCol, g_fxParam[3].xyz, saturate(drift * 0.7 + ridge * 0.35));

    float3 deepLacquer = aurCol * (0.22 + 0.78 * ridge) * g_fxParam[1].w;
    float3 paint = lerp(deepLacquer, chrome * (0.45 + 0.55 * aurCol) + aurCol * ridge * g_fxParam[1].w, g_fxParam[0].z);
    paint += aurCol * (fres * (0.55 + 0.85 * ridge + 0.65 * drift) * g_fxParam[1].w);

    float4 liv = Livery(i);
    float livAlpha = saturate(liv.a * g_fxParam[2].w);
    float3 finalCol = lerp(paint, liv.rgb * (0.65 + 0.65 * fres + 0.3 * spec), livAlpha);

    return float4(finalCol, 1.0);
}
