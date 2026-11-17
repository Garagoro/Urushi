// Initial D Manga Ink & Anime Overlay Shader
// Overlays crisp manga pen outlines, geometric panel crease inking, cel shadow toning,
// and anime glints ON TOP of the game's native car paint and liveries with proper Z-buffer testing.
// Premultiplied alpha output for ONE / INV_SRC_ALPHA blending.
//
// @param inkOpacity   p0.x 0.0..1.0 = 0.85 : Overall anime overlay opacity
// @param silhouette   p0.y 0.02..0.45 = 0.18 : Silhouette contour thickness
// @param creaseReach  p0.z 0.2..8.0 = 2.2 : Body crease & panel line sensitivity
// @param shadowTone   p0.w 0.0..1.0 = 0.40 : Manga cel shadow tone strength
// @color inkColor     p1.xyz = 0.01 0.01 0.02 : Comic ink color (black)
// @param glintPower   p1.w 0.0..1.0 = 0.70 : Anime specular shine
// @mode overlay
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);

    // 1. Geometric crease detection (door lines, spoiler edges, hood seams, fender creases)
    // Curvature computed via screen derivatives and normalized by world pixel size
    float3 dpdx = ddx(i.posRelCam);
    float3 dpdy = ddy(i.posRelCam);
    float pixelSize = max(max(length(dpdx), length(dpdy)), 1e-6);
    float curvature = (length(ddx(n)) + length(ddy(n))) / pixelSize;

    // Logarithmic ramp (from glow_edge.hlsl) separates subtle curves from sharp geometric seams
    float reach = clamp(g_fxParam[0].z, 0.2, 8.0);
    float lo = 3.5;
    float hi = lo * pow(50.0, lerp(0.4, 2.0, reach / 8.0));
    float creaseBand = saturate((log2(max(curvature, 1e-6)) - log2(lo)) / max(log2(hi) - log2(lo), 1e-4));
    float creaseInk = pow(creaseBand, 1.6);

    // 2. Silhouette manga ink contour (G-pen outer line from inner_glow.hlsl)
    // abs(dot) prevents back-facing geometry from turning solid black
    float silWidth = clamp(g_fxParam[0].y, 0.02, 0.45);
    float facing = saturate(abs(dot(n, -v)));
    float silBand = saturate(1.0 - smoothstep(0.0, silWidth, facing));
    float silInk = pow(silBand, 2.2);

    // Combine ink lines:
    float inkLine = saturate(max(creaseInk, silInk));

    // 3. Anime 2-tone cel shadow (smooth stepped darkening, no pixel raster noise)
    float3 lightDir = normalize(float3(0.35, 0.85, 0.40));
    float ndotl = dot(n, lightDir);
    float shadowTone = (1.0 - smoothstep(0.0, 0.16, ndotl)) * saturate(g_fxParam[0].w);

    // 4. Stylized anime specular highlight glint
    float3 h = normalize(lightDir + v);
    float ndoth = saturate(dot(n, h));
    float glint = pow(ndoth, 45.0) * saturate(g_fxParam[1].w);

    // 5. Premultiplied alpha composition for ONE / INV_SRC_ALPHA blend
    // Inking darkens underlying paint via Dst * (1 - inkTotal)
    // Glint adds sharp white specular highlights
    float opacity = saturate(g_fxParam[0].x);
    float inkTotal = saturate(inkLine + shadowTone * 0.35) * opacity;
    float3 inkColor = saturate(g_fxParam[1].xyz);
    float glintAlpha = saturate(glint) * opacity;
    float3 glintColor = float3(1.0, 1.0, 1.0);

    float finalAlpha = saturate(inkTotal + glintAlpha);
    float3 premulRgb = inkColor * inkTotal + glintColor * glintAlpha;

    return float4(premulRgb, finalAlpha);
}
