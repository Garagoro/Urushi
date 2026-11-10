// CarFX (CarX-WFX) common HLSL header for CarX Drift Racing Online 2
// Mirrors WeaponFX ergonomics on top of CarX DR2's MAT_CAR_PAINT_3_0 Forward pass.
// Include first in every shader:
//     #include "carfx.hlsli"
//     float4 main(PSIn i) : SV_Target { ... }

// Exact PS input signature of RenderCore/Standard (MAT_CAR_PAINT_3_0 Forward pass).
// Keep member order & semantics untouched so registers match CarX's vertex shader output.
struct PSIn {
    float2 uv        : TEXCOORD20;   // v0.xy  : mesh/livery UV0
    float  vcolW     : TEXCOORD21;   // v0.z   : vertex COLOR.w
    float  vcolX     : TEXCOORD22;   // v0.w   : vertex COLOR.x
    float3 posRelCam : TEXCOORD0;    // v1.xyz : worldPos - cameraPos
    float3 normalW   : TEXCOORD1;    // v2.xyz : world-space normal
    float4 tangentW  : TEXCOORD2;    // v3.xyzw: world-space tangent (xyz) + sign (w)
    float4 svpos     : SV_Position;  // v4.xy  : screen pixel coordinates
};

// CarX UnityPerMaterial cbuffer still bound on b6 during ForwardPass
cbuffer CarXPerMaterial : register(b6) {
    float4 g_carColor;            // u_vColor
    float4 g_carSecondaryColor;   // u_vSecondaryColor
    float4 g_carThirdColor;       // u_vThirdColor
    float4 g_carUnused3;
    float4 g_carParams;           // u_fParams
    float4 g_carParams1;          // u_fParams1
    float4 g_carParams2;          // u_fParams2
};

// CarFX host constant buffer on b13 (unused by RenderCore/Standard which uses b0..b11)
cbuffer CarFx : register(b13) {
    float4 g_fxTime;       // x: seconds, y: deltaTime, z: matchedDrawIndex, w: target (0=body, 1=glass)
    float4 g_fxScreen;     // x: vpWidth, y: vpHeight, z: 1/rtWidth, w: 1/rtHeight
    float4 g_fxParam[8];   // p0..p7 bound from @param / @color comments
    float4 g_fxFrameInfo;  // x: liveryBound (0/1), y: tex0Bound, z: tex1Bound, w: mode (1=replace, 2=overlay)
    float4 g_fxGame[4];    // 16 telemetry inputs pushed per-frame from Lua/C++
};

// Custom SRV bindings on t32..t38 (RenderCore/Standard uses up to t28)
Texture2D g_fxScene  : register(t32);   // Scene snapshot behind the car (copied if shader reads g_fxScene)
Texture2D g_fxTex0   : register(t33);   // User @tex 0
Texture2D g_fxTex1   : register(t34);   // User @tex 1
Texture2D g_fxTex2   : register(t35);   // User @tex 2
Texture2D g_fxTex3   : register(t36);   // User @tex 3
Texture2D g_fxDepth  : register(t37);   // Depth snapshot before car (copied if shader reads g_fxDepth)
Texture2D g_fxLivery : register(t38);   // Baked 2048x2048 livery atlas (rgb = decals, a = vinyl mask)

SamplerState g_linear : register(s14);  // Dedicated CarFX linear-wrap sampler
SamplerState g_point  : register(s15);  // Dedicated CarFX point-clamp sampler

float3 Normal(PSIn i) {
    return normalize(i.normalW);
}

float3 ViewDir(PSIn i) {
    return normalize(-i.posRelCam);
}

float Fresnel(PSIn i, float power) {
    float ndotv = saturate(dot(Normal(i), ViewDir(i)));
    return pow(1.0 - ndotv, max(power, 0.01));
}

float2 ScreenUV(PSIn i) {
    return i.svpos.xy * g_fxScreen.zw;
}

float2 ScreenNormal(PSIn i) {
    float3 n = Normal(i);
    float3 v = ViewDir(i); // fragment -> camera
    float3 upRef = abs(v.y) > 0.99 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 camRight = normalize(cross(v, upRef));
    float3 camUp = normalize(cross(camRight, v));
    return float2(dot(n, camRight), -dot(n, camUp));
}

float4 Livery(PSIn i) {
    return g_fxFrameInfo.x > 0.5 ? g_fxLivery.Sample(g_linear, i.uv) : float4(0, 0, 0, 0);
}

float LiveryMask(PSIn i) {
    return saturate(Livery(i).a);
}

float4 SceneAt(PSIn i, float2 pixelOffset) {
    float2 uv = (i.svpos.xy + pixelOffset) * g_fxScreen.zw;
    return g_fxScene.SampleLevel(g_linear, saturate(uv), 0);
}

float3 SceneAtSafe(PSIn i, float2 pixelOffset) {
    float3 c = SceneAt(i, pixelOffset).rgb;
    [unroll] for (int k = 0; k < 3; ++k) {
        if (dot(c, c) >= 1e-6) break;
        float frac = (k == 0) ? 0.5 : (k == 1 ? 0.25 : 0.0);
        c = SceneAt(i, pixelOffset * frac).rgb;
    }
    return c;
}

float Hash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float3 Hash33(float3 p) {
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float Noise3(float3 p) {
    float3 ip = floor(p);
    float3 fp = frac(p);
    fp = fp * fp * (3.0 - 2.0 * fp);
    float n000 = Hash13(ip + float3(0, 0, 0));
    float n100 = Hash13(ip + float3(1, 0, 0));
    float n010 = Hash13(ip + float3(0, 1, 0));
    float n110 = Hash13(ip + float3(1, 1, 0));
    float n001 = Hash13(ip + float3(0, 0, 1));
    float n101 = Hash13(ip + float3(1, 0, 1));
    float n011 = Hash13(ip + float3(0, 1, 1));
    float n111 = Hash13(ip + float3(1, 1, 1));
    float nx00 = lerp(n000, n100, fp.x);
    float nx10 = lerp(n010, n110, fp.x);
    float nx01 = lerp(n001, n101, fp.x);
    float nx11 = lerp(n011, n111, fp.x);
    return lerp(lerp(nx00, nx10, fp.y), lerp(nx01, nx11, fp.y), fp.z);
}

float Fbm3(float3 p) {
    float a = 0.5, s = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
        s += a * Noise3(p);
        p *= 2.03;
        a *= 0.5;
    }
    return s;
}

float DepthAt(PSIn i) {
    return g_fxDepth.SampleLevel(g_point, saturate(ScreenUV(i)), 0).r;
}

float4 Tex0(PSIn i) { return g_fxTex0.Sample(g_linear, i.uv); }
float4 Tex1(PSIn i) { return g_fxTex1.Sample(g_linear, i.uv); }
float4 Tex2(PSIn i) { return g_fxTex2.Sample(g_linear, i.uv); }
float4 Tex3(PSIn i) { return g_fxTex3.Sample(g_linear, i.uv); }

// Car telemetry mapped by scripts/carfx_mod.lua
float CarSpeedKmh()  { return g_fxGame[0].x; }
float CarRpm()       { return g_fxGame[0].y; }
float CarDriftDeg()  { return g_fxGame[0].z; }
float CarDriftNorm() { return g_fxGame[0].w; } // 0..1 smooth drift intensity
float CarThrottle()  { return g_fxGame[1].x; }
float CarBrake()     { return g_fxGame[1].y; }
float CarGear()      { return g_fxGame[1].z; }
