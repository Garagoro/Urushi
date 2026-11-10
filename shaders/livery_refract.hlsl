// Stealth Predator / Glass Refraction: bends the real background scene behind the car using screen normals + Fresnel rim.
// @param refractPx  p0.x 0..120 = 42.0 : screen refraction offset in pixels
// @param edgeGlow   p0.y 0..6   = 2.4  : Fresnel rim brightness
// @param keepLivery p0.z 0..1   = 0.9  : keep painted vinyl stickers solid on top of cloaked body
// @color rimTint    p1.xyz = 0.20 0.85 1.0 : edge energy colour
// @color glassTint  p2.xyz = 0.82 0.94 1.0 : glass tint multiplying scene
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float fres = Fresnel(i, 3.0);

    float2 off = ScreenNormal(i) * g_fxParam[0].x * (g_fxScreen.x / 2560.0) * (1.0 + 0.5 * CarDriftNorm());
    float3 bgR = SceneAtSafe(i, off * 1.12);
    float3 bgG = SceneAtSafe(i, off * 1.00);
    float3 bgB = SceneAtSafe(i, off * 0.88);
    float3 glass = float3(bgR.r, bgG.g, bgB.b) * g_fxParam[2].xyz + g_fxParam[1].xyz * fres * g_fxParam[0].y;

    float4 liv = Livery(i);
    float3 col = lerp(glass, liv.rgb * (0.65 + 0.5 * fres), saturate(liv.a * g_fxParam[0].z));
    return float4(col, 1.0);
}
