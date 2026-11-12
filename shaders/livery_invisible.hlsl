// Invisible body (predator / cloak):
// the body is replaced by the picture BEHIND the car (road, kerbs, sky), so you see the asphalt instead of the paint.
// `@stealth 1` makes URUSHI draw the body BEFORE the engine / chassis / interior and hide them behind it,
// so the scene snapshot is clean and nothing of the car leaks through. Wheels, lights and glass stay visible.
// Defaults are almost perfectly clear; raise `shimmer` / `edgeBend` / `rim` for the "predator" look.
// @stealth 1
// @param edgeBend   p0.x 0..60  = 5.0  : background bend at the silhouette edges (px)
// @param shimmer    p0.y 0..1   = 0.0  : heat-haze ripple across the body
// @param speed      p0.z 0..4   = 1.2  : ripple speed
// @param rim        p0.w 0..2   = 0.0  : outline glow so the silhouette stays readable
// @color rimColor   p1.xyz = 0.45 0.88 1.0 : outline color
// @param keepLivery p1.w 0..1   = 0.0  : 1 = vinyls stay painted on the invisible body
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float bendPx  = g_fxParam[0].x;
    float shimmer = saturate(g_fxParam[0].y);
    float speed   = g_fxParam[0].z;
    float rim     = g_fxParam[0].w;
    float3 rimCol = g_fxParam[1].xyz;
    float keepLiv = saturate(g_fxParam[1].w);

    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float cv = saturate(dot(n, v));
    float edge = pow(1.0 - cv, 3.0);                  // 0 face-on .. 1 at the silhouette
    float scale = g_fxScreen.x / 2560.0;

    // only the edges bend the background, the middle of the panels is pixel-perfect see-through
    float2 off = ScreenNormal(i) * (bendPx * scale * edge);
    if (shimmer > 0.001) {
        float t = g_fxTime.x * speed;
        float3 wp = i.posRelCam * 1.3;
        float2 w = float2(Noise3(wp + float3(0.0, t * 0.7, t)),
                          Noise3(wp + float3(7.3, t * 0.5, -t * 0.9))) - 0.5;
        off += w * (shimmer * 16.0 * scale);
    }

    float3 col = SceneAtSafe(i, off);
    col += rimCol * (edge * rim);

    if (keepLiv > 0.001) {
        float4 liv = Livery(i);
        col = lerp(col, liv.rgb, saturate(liv.a * keepLiv));
    }
    return float4(col, 1.0);
}
