// Chameleon Holo Paint: view-angle iridescent gradient + digital scanlines reacting to speed and drift angle.
// @param shiftFreq  p0.x 0.5..6 = 2.2  : rainbow colour cycling rate
// @param scanDens   p0.y 10..200 = 80  : holographic scanline density
// @param keepLivery p0.z 0..1  = 0.7   : blend original vinyl decals through hologram
// @param driftReact p0.w 0..3  = 1.5   : boost chroma shift during drift
// @color baseTint   p1.xyz = 0.08 0.12 0.25 : base shadow tone
// @param intensity  p1.w 0.2..5 = 1.6  : emission multiplier
// @mode replace
#include "carfx.hlsli"

float3 Spectrum(float x) {
    return 0.5 + 0.5 * cos(6.28318 * (x + float3(0.0, 0.33, 0.67)));
}

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float ndotv = saturate(dot(n, v));
    float fres = pow(1.0 - ndotv, 2.2);

    float phase = (1.0 - ndotv) * g_fxParam[0].x + g_fxTime.x * 0.35 + CarDriftNorm() * g_fxParam[0].w * 0.45;
    float3 holo = Spectrum(phase + i.posRelCam.y * 0.25);

    float scan = 0.85 + 0.15 * sin(i.svpos.y * 0.5 + i.uv.y * g_fxParam[0].y - g_fxTime.x * 6.0);
    float3 paint = lerp(g_fxParam[1].xyz, holo, saturate(0.35 + fres * 0.85)) * scan;

    float4 liv = Livery(i);
    float3 liveryLit = liv.rgb * (0.55 + 0.75 * holo);
    float3 res = lerp(paint, liveryLit, saturate(liv.a * g_fxParam[0].z));

    return float4(res * g_fxParam[1].w, 1.0);
}
