// Invisible Car Body Cloak (True Optical Camouflage):
// Cloaks ONLY the painted body panels (`car_body`) to refract pure background world/road/sky via queue ordering (@stealth 1),
// while keeping interior & driver visible through the windows and preserving wheels, suspension arms, brakes, lights & external trim!
// @stealth 1
// @param refraction   p0.x 0..100 = 26.0 : optical background bend in pixels
// @param dispersion   p0.y 0..1   = 0.10 : subtle RGB prism split at curved panels
// @param shimmer      p0.z 0..1   = 0.25 : Predator active-camo heat ripple
// @param rimGlow      p0.w 0..4   = 0.60 : delicate contour glow so silhouette stays readable
// @color tint         p1.xyz = 0.96 0.99 1.0 : optical pass-through tint (near 1 = invisible)
// @param edgeDark     p1.w 0..1.5 = 0.14 : subtle glass-edge darkening
// @param reflection   p2.x 0..1.5 = 0.28 : clear sky/sun grazing sheen
// @param keepLivery   p2.y 0..1   = 0.0  : 0 = fully invisible car, 1 = floating vinyl decals
// @param speed        p2.z 0..4   = 1.2  : shimmer wave speed
// @color rimColor     p3.xyz = 0.45 0.88 1.0 : stealth rim highlight color
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float bendPx   = g_fxParam[0].x;
    float disp     = g_fxParam[0].y;
    float shimmer  = saturate(g_fxParam[0].z);
    float rimGlow  = g_fxParam[0].w;
    float3 tint    = g_fxParam[1].xyz;
    float edgeDark = g_fxParam[1].w;
    float reflK    = g_fxParam[2].x;
    float keepLiv  = saturate(g_fxParam[2].y);
    float speed    = g_fxParam[2].z;
    float3 rimCol  = g_fxParam[3].xyz;

    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float cv = saturate(dot(n, v));
    float scale = g_fxScreen.x / 2560.0;
    float fres = pow(1.0 - cv, 4.0);

    float2 off = ScreenNormal(i) * (bendPx * scale * (0.25 + 0.75 * fres));
    if (shimmer > 0.001) {
        float t = g_fxTime.x * speed;
        float3 wp = i.posRelCam * 1.1;
        float2 wave = float2(
            Noise3(wp + float3(0.0, t * 0.7, t)),
            Noise3(wp + float3(7.3, t * 0.5, -t * 0.9))
        ) - 0.5;
        off += wave * (shimmer * 22.0 * scale * (1.0 + 0.6 * CarDriftNorm()));
    }

    float3 seen;
    seen.r = SceneAtSafe(i, off * (1.0 + disp)).r;
    seen.g = SceneAtSafe(i, off).g;
    seen.b = SceneAtSafe(i, off * (1.0 - disp)).b;

    // Preserve natural scene brightness while tinting edges very lightly
    float3 col = seen * lerp(float3(1.0, 1.0, 1.0), tint, 0.35 + 0.65 * (1.0 - cv));
    col *= 1.0 - saturate(edgeDark * 0.45 * pow(1.0 - cv, 2.5));

    // Faint grazing sky/sun sheen + stealth edge shimmer
    float3 reflDir = reflect(-v, n);
    float skyGrad = saturate(reflDir.y * 0.5 + 0.5);
    float3 skyEnv = lerp(float3(0.22, 0.25, 0.28), float3(0.70, 0.86, 1.05), pow(skyGrad, 1.3));
    float spec = pow(saturate(dot(reflDir, normalize(float3(0.4, 0.8, 0.3)))), 64.0) * 1.1;
    float schlick = 0.04 + 0.96 * pow(1.0 - cv, 5.0);
    col = lerp(col, skyEnv + spec, saturate(schlick * reflK));
    col += rimCol * (fres * rimGlow * (0.35 + 0.65 * CarDriftNorm()));

    // Optional floating vinyl stickers (keepLivery = 0 by default for pure stealth invisibility)
    if (keepLiv > 0.001) {
        float4 liv = Livery(i);
        col = lerp(col, liv.rgb * (0.7 + 0.4 * schlick), saturate(liv.a * keepLiv));
    }
    return float4(col, 1.0);
}
