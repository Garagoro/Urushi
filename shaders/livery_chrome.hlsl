// Liquid Chrome + Livery Preserve: analytic HDR studio reflection across car body with optional original vinyl overlay.
// @param polish     p0.x 1..16 = 6.0  : highlight sharpness
// @param keepLivery p0.y 0..1  = 0.85 : keep painted vinyls visible over chrome
// @param sheenSpeed p0.z 0..5  = 1.2  : moving studio light bar speed
// @param roughness  p0.w 0..1  = 0.12 : surface micro-roughness tint
// @color tint       p1.xyz = 0.92 0.96 1.0 : chrome tint
// @param exposure   p1.w 0.2..4 = 1.4 : overall brightness
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(-v, n);

    float t = g_fxTime.x * g_fxParam[0].z;
    float horizon = smoothstep(-0.18, 0.22, r.y);
    float3 sky = lerp(float3(0.08, 0.10, 0.16), float3(0.75, 0.88, 1.10), pow(saturate(r.y * 0.5 + 0.5), 1.4));
    float3 ground = lerp(float3(0.03, 0.03, 0.04), float3(0.14, 0.16, 0.20), saturate(-r.y));

    float3 lightDir = normalize(float3(sin(t) * 0.7, 0.65, cos(t) * 0.7));
    float spec = pow(saturate(dot(r, lightDir)), g_fxParam[0].x * 8.0) * 2.5;
    float strip = pow(saturate(1.0 - abs(r.y - 0.35 - 0.12 * sin(t + r.x * 2.0)) * 4.0), 3.0) * 1.4;

    float fres = lerp(0.65, 1.0, Fresnel(i, 3.0));
    float3 metal = lerp(ground, sky, horizon) * g_fxParam[1].xyz * fres + (spec + strip) * g_fxParam[1].xyz;
    metal = lerp(metal, g_carColor.rgb * 0.35 + metal * 0.65, g_fxParam[0].w);

    float4 liv = Livery(i);
    float livAlpha = saturate(liv.a * g_fxParam[0].y);
    float3 finalCol = lerp(metal, liv.rgb * (0.45 + fres * 0.8 + spec * 0.3), livAlpha);

    return float4(finalCol * g_fxParam[1].w, 1.0);
}
