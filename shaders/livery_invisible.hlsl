// Invisible body (predator / cloak) without touching the draw order.
// The car's engine, chassis and interior are already in the scene snapshot under the body, so reading "the pixel behind"
// would show them. Instead the body shows what is BESIDE the car: the shader projects the car's 3D box to the screen
// and, for every body pixel, reads the pixel mirrored across the nearest left/right edge of that box. Same screen row, so
// the horizon and road layers continue naturally, and the picture is seamless at the silhouette. Needs the camera pose
// pushed by the host (g_fxCam*); until then the body just shows the scene as is.
// @param margin     p0.x 0..0.5 = 0.06 : grow the car box so no body pixel samples the car itself
// @param edgeBend   p0.y 0..60  = 4.0  : background bend at the silhouette edges (px)
// @param shimmer    p0.z 0..1   = 0.0  : heat-haze ripple across the body
// @param speed      p0.w 0..4   = 1.2  : ripple speed
// @param rim        p1.x 0..2   = 0.0  : outline glow so the silhouette stays readable
// @param keepLivery p1.y 0..1   = 0.0  : 1 = vinyls stay painted on the invisible body
// @color rimColor   p2.xyz = 0.45 0.88 1.0 : outline color
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float margin  = g_fxParam[0].x;
    float bendPx  = g_fxParam[0].y;
    float shimmer = saturate(g_fxParam[0].z);
    float speed   = g_fxParam[0].w;
    float rim     = g_fxParam[1].x;
    float keepLiv = saturate(g_fxParam[1].y);
    float3 rimCol = g_fxParam[2].xyz;

    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float edge = pow(1.0 - saturate(dot(n, v)), 3.0);
    float scale = g_fxScreen.x / 2560.0;

    float2 off = float2(0.0, 0.0);
    bool ok = HasCamBasis() && HasCarSpatial();

    float minX = 1e9, maxX = -1e9;
    if (ok) {
        float f = CamFocalPx(i);
        float3 ctr = g_fxCarPos.xyz;
        float hw = g_fxCarPos.w * (1.0 + margin) + 0.1;
        float hl = g_fxCarRight.w * (1.0 + margin) + 0.1;
        float ht = g_fxCarUp.w * (1.0 + margin);
        [unroll] for (int k = 0; k < 8; ++k) {
            float sx = (k & 1) ? 1.0 : -1.0;
            float sz = (k & 2) ? 1.0 : -1.0;
            float sy = (k & 4) ? ht : -0.25;
            float3 pr = CamProject(ctr + g_fxCarRight.xyz * (sx * hw) + g_fxCarFwd.xyz * (sz * hl) + g_fxCarUp.xyz * sy, f);
            if (pr.z < 0.05) ok = false;          // car box crosses the camera plane: no reliable projection
            minX = min(minX, pr.x);
            maxX = max(maxX, pr.x);
        }
    }

    if (ok) {
        float W = 1.0 / g_fxScreen.z;
        float x = i.svpos.x;
        float dl = max(x - minX, 0.0);
        float dr = max(maxX - x, 0.0);
        float xl = minX - dl;
        float xr = maxX + dr;
        bool okL = xl >= 0.0;
        bool okR = xr <= W - 1.0;
        float xs;
        if (okL && okR)  xs = (dl < dr) ? xl : xr;
        else if (okL)    xs = xl;
        else if (okR)    xs = xr;
        else             xs = clamp((dl < dr) ? xl : xr, 0.0, W - 1.0);
        off.x = xs - x;
    }

    // predator touches: edge bend + optional heat haze
    off += ScreenNormal(i) * (bendPx * scale * edge);
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
