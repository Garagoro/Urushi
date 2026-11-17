// @param intensity p0.x 0.0..1.0 = 0.0 : Speed line intensity
// @param density   p0.y 10.0..120.0 = 50.0 : Line density / count
// @param speed     p0.z 1.0..30.0 = 12.0 : Animation speed
// @param innerRad  p0.w 0.1..0.8 = 0.35 : Clear center radius
// @param punch     p1.x 0.0..0.02 = 0.005 : Chromatic punch
// @param vignette  p1.y 0.0..1.0 = 0.4 : Vignette strength
// @color lineColor p2.xyz = 1.0 1.0 1.0 : Speed lines color

cbuffer PostFx : register(b0) {
    float4 g_time;        // x: time (s), y: deltaTime (s), z: frame, w: 0
    float4 g_screen;      // x: width, y: height, z: 1/width, w: 1/height
    float4 g_param[8];    // p0..p7
    float4 g_driftState;  // x: abs angle, y: signed angle, z: norm65, w: speed km/h
};

#define p0 g_param[0]
#define p1 g_param[1]
#define p2 g_param[2]

Texture2D g_scene   : register(t0);
SamplerState g_smp  : register(s0);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target {
    float intensity = saturate(p0.x);
    if (intensity <= 0.0001) {
        return g_scene.SampleLevel(g_smp, i.uv, 0);
    }

    // Aspect-ratio corrected center offset
    float2 c = i.uv - 0.5;
    float aspect = (g_screen.y > 0.0) ? (g_screen.x / g_screen.y) : 1.7777;
    float2 ca = float2(c.x * aspect, c.y);
    float r = length(ca);
    float theta = atan2(ca.y, ca.x);

    // Stepped frame time for anime / manga hand-drawn feel
    float stepRate = max(p0.z, 1.0);
    float timeStep = floor(g_time.x * stepRate);

    // Angular sector and line indexing
    float normAngle = (theta + 3.14159265) / 6.2831853;
    float lineCount = max(p0.y, 10.0);
    float rayIdx = floor(normAngle * lineCount);

    // Hash pseudo-randomness for this sector and time frame
    float h1 = frac(sin(rayIdx * 12.9898 + timeStep * 78.233) * 43758.5453);
    float h2 = frac(sin(rayIdx * 39.346 + timeStep * 11.135) * 23421.631);

    // Ray presence threshold (classic manga spacing)
    float activeRay = (h1 > 0.65) ? 1.0 : 0.0;

    // Angular ray shape and thickness
    float fracAngle = frac(normAngle * lineCount) - 0.5;
    float halfWidth = lerp(0.08, 0.24, h2);
    float lineShape = 1.0 - smoothstep(0.0, halfWidth, abs(fracAngle));

    // Radial start and fade
    float innerR = p0.w + h1 * 0.12;
    float radialFade = smoothstep(innerR, innerR + 0.22, r);

    float speedLine = lineShape * radialFade * activeRay;

    // Chromatic punch: slight radial aberration under drift
    float punchAmount = p1.x * intensity * r;
    float2 punchDir = (r > 0.0001) ? normalize(c) : float2(0, 0);

    float3 col;
    col.r = g_scene.SampleLevel(g_smp, i.uv - punchDir * punchAmount, 0).r;
    col.g = g_scene.SampleLevel(g_smp, i.uv, 0).g;
    col.b = g_scene.SampleLevel(g_smp, i.uv + punchDir * punchAmount, 0).b;

    // Vignette
    float vig = 1.0 - smoothstep(0.35, 0.95, r) * p1.y * intensity;
    col *= vig;

    // Blend speed lines over scene
    float lineAlpha = saturate(speedLine * intensity * 1.6);
    col = lerp(col, p2.xyz, lineAlpha);

    return float4(col, 1.0);
}
