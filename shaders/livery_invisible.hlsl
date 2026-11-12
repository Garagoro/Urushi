// Glass / invisible body. `@stealth 1` makes URUSHI hide the car's internals (engine, suspension, interior...) from the
// colour pass - they keep casting shadows but draw nothing - so the scene snapshot behind the body is the real road.
// The body then shows that road through a thin lens: it bends at the edges, splits into a faint prism, and a Fresnel rim
// keeps the silhouette readable. Wheels, brakes and lights stay. Defaults are almost perfectly clear.
// @stealth 1
// @param bend       p0.x 0..80  = 10.0 : background bend (px) - grows towards the silhouette
// @param dispersion p0.y 0..1   = 0.12 : prism RGB split
// @param shimmer    p0.z 0..1   = 0.0  : heat-haze ripple across the body
// @param speed      p0.w 0..4   = 1.2  : ripple speed
// @param rim        p1.x 0..2   = 0.35 : outline glow
// @param reflection p1.y 0..1.5 = 0.35 : sky / sun sheen at grazing angles
// @param keepLivery p1.z 0..1   = 0.0  : 1 = vinyls stay painted on the glass
// @param tintAmount p1.w 0..1   = 0.15 : cool glass tint towards the edges
// @color rimColor   p2.xyz = 0.45 0.88 1.0 : outline color
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float bendPx  = g_fxParam[0].x;
    float disp    = g_fxParam[0].y;
    float shimmer = saturate(g_fxParam[0].z);
    float speed   = g_fxParam[0].w;
    float rim     = g_fxParam[1].x;
    float reflK   = g_fxParam[1].y;
    float keepLiv = saturate(g_fxParam[1].z);
    float tintK   = saturate(g_fxParam[1].w);
    float3 rimCol = g_fxParam[2].xyz;

    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float cv = saturate(dot(n, v));
    float edge = pow(1.0 - cv, 2.5);
    float scale = g_fxScreen.x / 2560.0;

    // lens: the background bends along the surface normal, more at grazing angles
    float2 off = ScreenNormal(i) * (bendPx * scale * (0.2 + 0.8 * edge));
    if (shimmer > 0.001) {
        float t = g_fxTime.x * speed;
        float3 wp = i.posRelCam * 1.3;
        float2 w = float2(Noise3(wp + float3(0.0, t * 0.7, t)),
                          Noise3(wp + float3(7.3, t * 0.5, -t * 0.9))) - 0.5;
        off += w * (shimmer * 16.0 * scale);
    }

    float3 seen;
    seen.r = SceneAtSafe(i, off * (1.0 + disp)).r;
    seen.g = SceneAtSafe(i, off).g;
    seen.b = SceneAtSafe(i, off * (1.0 - disp)).b;

    float3 col = seen * lerp(float3(1.0, 1.0, 1.0), float3(0.82, 0.94, 1.0), tintK * edge);

    // grazing sky / sun sheen
    float3 r = reflect(-v, n);
    float sky = saturate(r.y * 0.5 + 0.5);
    float3 env = lerp(float3(0.20, 0.23, 0.27), float3(0.65, 0.84, 1.05), pow(sky, 1.3));
    float spec = pow(saturate(dot(r, normalize(float3(0.4, 0.8, 0.3)))), 64.0);
    float schlick = 0.04 + 0.96 * pow(1.0 - cv, 5.0);
    col = lerp(col, env + spec, saturate(schlick * reflK));
    col += rimCol * (edge * rim);

    if (keepLiv > 0.001) {
        float4 liv = Livery(i);
        col = lerp(col, liv.rgb, saturate(liv.a * keepLiv));
    }
    return float4(col, 1.0);
}
