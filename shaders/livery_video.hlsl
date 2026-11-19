// @mode replace
// @tex 0 videoTex colour : Animated video or image sequence
// @param uvScaleX      p0.x 0.05..10.0 = 1.0    : Horizontal UV Scale
// @param uvScaleY      p0.y 0.05..10.0 = 1.0    : Vertical UV Scale
// @param uvOffsetX     p0.z -2.0..2.0 = 0.0     : Horizontal UV Offset
// @param uvOffsetY     p0.w -2.0..2.0 = 0.0     : Vertical UV Offset
// @param uvRotation    p1.x -180.0..180.0 = 0.0 : UV Rotation Angle (degrees)
// @param wrapMode      p1.y 0.0..1.0 = 1.0      : Tiling Mode (1.0 = Repeat, 0.0 = Clamp)
// @param brightness    p1.z 0.0..3.0 = 1.0      : Brightness Multiplier
// @param emissiveBoost p1.w 0.0..5.0 = 0.0      : Emissive Neon Boost
// @param paintBlend    p2.x 0.0..1.0 = 0.0      : Blend with Base Car Paint (0 = video only)
// @param keepDecals    p2.y 0.0..1.0 = 1.0      : Preserve Livery Decals on Top
// @color basePaint     p3.xyz = 0.05 0.05 0.05  : Base Paint Color (when blended/clamped)
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    // 1. Transform UV coordinates around center (0.5, 0.5)
    float2 centered = i.uv - float2(0.5, 0.5);
    float rad = radians(p1.x);
    float s = sin(rad);
    float c = cos(rad);
    float2 rotated = float2(centered.x * c - centered.y * s, centered.x * s + centered.y * c);

    float2 scale = max(float2(p0.x, p0.y), float2(0.001, 0.001));
    float2 transformedUV = (rotated / scale) + float2(0.5, 0.5) + float2(p0.z, p0.w);

    // 2. Check clamp mode boundary
    bool isClamped = (p1.y < 0.5);
    bool outOfBounds = isClamped && (transformedUV.x < 0.0 || transformedUV.x > 1.0 ||
                                     transformedUV.y < 0.0 || transformedUV.y > 1.0);

    // 3. Sample animated user texture 0 (g_fxTex0)
    float4 videoSample = outOfBounds ? float4(0.0, 0.0, 0.0, 0.0) : g_fxTex0.Sample(g_linear, transformedUV);

    // Video color with brightness & emissive boost
    float3 videoColor = videoSample.rgb * p1.z + videoSample.rgb * p1.w;

    // 4. Base car paint with subtle Fresnel reflection
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float fresnel = Fresnel(i, 3.0);
    float3 paintColor = p3.xyz + fresnel * 0.15;

    // 5. Blend video livery with base paint
    // If out of bounds in clamp mode, show base paint
    float blendFactor = outOfBounds ? 1.0 : saturate(p2.x);
    float3 surfaceColor = lerp(videoColor, paintColor, blendFactor);

    // 6. Optionally preserve player's baked livery decals on top
    float4 livery = g_fxLivery.Sample(g_linear, i.uv);
    float decalAlpha = saturate(livery.a * p2.y);
    float3 finalColor = lerp(surfaceColor, livery.rgb, decalAlpha);

    return float4(finalColor, 1.0);
}
