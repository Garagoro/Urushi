// Livery Neon Overlay: adds glowing energy waves and edge highlight strictly on the baked vinyl decals (or whole body).
// @param speed      p0.x 0..8 = 2.2   : wave animation speed
// @param freq       p0.y 0.5..20 = 6.0 : wave spatial frequency
// @param liveryOnly p0.z 0..1 = 1.0   : 1 = affect only vinyl stickers, 0 = whole body paint
// @param driftBoost p0.w 0..3 = 1.5   : extra intensity while drifting
// @color colorA     p1.xyz = 0.05 0.85 1.0 : primary neon colour
// @param brightness p1.w 0..8 = 2.8   : emission strength
// @color colorB     p2.xyz = 1.0 0.18 0.72 : secondary drift/crest colour
// @param fresnelPow p2.w 0.5..8 = 2.5 : rim/angle emphasis
// @mode overlay
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float mask = lerp(1.0, LiveryMask(i), saturate(g_fxParam[0].z));
    if (mask <= 0.002) discard;

    float t = g_fxTime.x * g_fxParam[0].x;
    float wave = 0.5 + 0.5 * sin(i.posRelCam.x * g_fxParam[0].y + i.posRelCam.z * (g_fxParam[0].y * 0.7) - t * 2.5);
    float scan = pow(wave, 3.0);

    float driftMix = saturate(CarDriftNorm() * g_fxParam[0].w);
    float fres = Fresnel(i, g_fxParam[2].w);

    float3 liveryRgb = Livery(i).rgb;
    float3 glowCol = lerp(g_fxParam[1].xyz, g_fxParam[2].xyz, saturate(scan * 0.65 + driftMix * 0.6));
    float3 outCol = lerp(liveryRgb, glowCol, 0.75) * (0.45 + 1.15 * scan + 0.85 * fres + driftMix);

    float alpha = saturate(mask * (0.28 + 0.62 * max(scan, fres)));
    float lum = g_fxParam[1].w * (1.0 + driftMix * 0.8);
    // Premultiplied alpha for CarFX ONE / INV_SRC_ALPHA overlay blend
    return float4(outCol * lum * alpha, alpha);
}
