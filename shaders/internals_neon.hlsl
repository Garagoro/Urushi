// Sample shader for the INTERNALS target (engine, suspension, interior): neon edge glow on dark metal.
// Load it into the internals slot, not the body slot.
// @param glow  p0.x 0..4 = 1.6 : edge glow strength
// @color neon  p1.xyz = 0.1 0.9 1.0 : glow color
// @mode replace
#include "carfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float fr = pow(1.0 - saturate(dot(n, v)), 2.5);
    float3 base = float3(0.015, 0.02, 0.03) + 0.05 * saturate(n.y * 0.5 + 0.5);
    return float4(base + g_fxParam[1].xyz * fr * g_fxParam[0].x, 1.0);
}
