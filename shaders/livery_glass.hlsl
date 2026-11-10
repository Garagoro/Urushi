// Crystal Car Glass (WeaponFX glass.hlsl port): real-scene refraction, prism dispersion,
// 12-tap golden-spiral frosted roughness blur, Beer-Lambert thickness tint, Schlick Fresnel & optional livery decals.
// @param refraction   p0.x 0..120 = 45.0  : refraction bend in pixels
// @param dispersion   p0.y 0..1   = 0.16  : prism RGB split fraction
// @param roughness    p0.z 0..1   = 0.0   : 0 = clear crystal, 1 = frosted glass
// @param grain        p0.w 0..1   = 0.45  : micro-grain texture in roughness
// @color tint         p1.xyz = 0.85 0.95 1.0 : glass absorption tint
// @param density      p1.w 0..4   = 0.85  : thickness deepening at grazing edges
// @param reflection   p2.x 0..2   = 1.1   : sky/sun Fresnel reflection strength
// @param ior          p2.y 1..2.5 = 1.52  : index of refraction (1.5 glass, 2.4 diamond)
// @param edgeDark     p2.z 0..2   = 0.45  : silhouette dark glass rim
// @param keepLivery   p2.w 0..1   = 0.95  : keep baked vinyl decals solid over glass
// @param ripple       p3.x 0..1   = 0.0   : animated liquid glass wave
// @param speed        p3.y 0..4   = 1.0   : wave animation speed
// @mode replace
#include "carfx.hlsli"

static const int kFrostTaps = 12;
static const float kFrostPx = 44.0;

float2 FrostTap(int k, float spin) {
    float a = k * 2.39996323 + spin;
    float r = sqrt((k + 0.5) / kFrostTaps);
    return float2(cos(a), sin(a)) * r;
}

float4 main(PSIn i) : SV_Target {
    float bendPx   = g_fxParam[0].x;
    float disp     = g_fxParam[0].y;
    float rough    = saturate(g_fxParam[0].z);
    float grain    = saturate(g_fxParam[0].w);
    float3 tint    = g_fxParam[1].xyz;
    float density  = g_fxParam[1].w;
    float reflK    = g_fxParam[2].x;
    float ior      = max(g_fxParam[2].y, 1.0);
    float edgeK    = g_fxParam[2].z;
    float keepLiv  = saturate(g_fxParam[2].w);
    float ripple   = saturate(g_fxParam[3].x);
    float speed    = g_fxParam[3].y;

    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float cv = saturate(dot(n, v));
    float scale = g_fxScreen.x / 2560.0;

    float f = rough;
    float fe = f * f;

    float2 off = ScreenNormal(i) * bendPx * scale;
    if (f > 0.001 && grain > 0.001) {
        float3 g = Hash33(floor(i.posRelCam * 30.0)) - 0.5;
        off += g.xy * (fe * grain * 10.0 * scale);
    }
    if (ripple > 0.001) {
        float t = g_fxTime.x * speed;
        float3 wp = i.posRelCam;
        float2 w = float2(Noise3(wp * 0.6 + float3(0, 0, t)), Noise3(wp * 0.6 + float3(5.2, 1.3, t * 0.8))) - 0.5;
        off += w * (ripple * 38.0 * scale);
    }

    float3 sharp;
    sharp.r = SceneAtSafe(i, off * (1.0 + disp)).r;
    sharp.g = SceneAtSafe(i, off).g;
    sharp.b = SceneAtSafe(i, off * (1.0 - disp)).b;
    float3 seen = sharp;

    if (f > 0.001) {
        float spin = Hash13(float3(i.svpos.xy, 0.0)) * 6.2831853 * lerp(0.25, 1.0, grain);
        float radius = fe * kFrostPx * scale;
        float3 blur = 0.0;
        [unroll] for (int k = 0; k < kFrostTaps; ++k) {
            blur += SceneAt(i, off + FrostTap(k, spin) * radius).rgb;
        }
        blur /= kFrostTaps;
        blur = lerp(blur, dot(blur, float3(0.333, 0.333, 0.333)) + 0.04, fe * 0.35);
        seen = lerp(sharp, blur, saturate(f * 8.0));
    }

    // Beer-Lambert glass absorption by thickness
    float path = 1.0 / max(cv, 0.2);
    seen *= pow(max(tint, 0.001), density * path);
    seen *= 1.0 - saturate(edgeK * 0.55 * pow(1.0 - cv, 3.0));

    // Schlick Fresnel sky/horizon reflection
    float f0 = (ior - 1.0) / (ior + 1.0);
    f0 = f0 * f0;
    float schlick = f0 + (1.0 - f0) * pow(1.0 - cv, 5.0);
    float3 reflDir = reflect(-v, n);
    float skyGrad = saturate(reflDir.y * 0.5 + 0.5);
    float3 skyEnv = lerp(float3(0.16, 0.19, 0.22), float3(0.55, 0.78, 1.05), pow(skyGrad, 1.4));
    float spec = pow(saturate(dot(reflDir, normalize(float3(0.4, 0.8, 0.3)))), lerp(96.0, 12.0, f)) * 1.4;
    float3 col = lerp(seen, skyEnv + spec, saturate(schlick * reflK * (1.0 - 0.5 * f)));

    // Keep vinyl decals painted on top of clear/frosted car body
    float4 liv = Livery(i);
    col = lerp(col, liv.rgb * (0.7 + 0.45 * schlick), saturate(liv.a * keepLiv));
    return float4(col, 1.0);
}
