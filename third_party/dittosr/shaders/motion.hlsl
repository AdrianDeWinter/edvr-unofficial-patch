// DittoSR's motion pass: the motion checks and the derived reactive mask. Render size - once per render pixel, not per output pixel
// (the upscale did them per output pixel: 8 reads of the motion at each, ratio^2 times over the same render pixels).
// u0 (RGBA8): x = the motion's change against its neighbours' (rj), y = the disocclusion, z = the
// derived reactive mask, w = this pixel's encoded luma - the next frame's reactive test reads it where the pixel went.
// t0 = the previous frame's motion pass, t1 = the convert pass's (this frame), t2 = the colour, t3 = the previous convert pass's.
#include "common.hlsli"

Texture2D<float4> PrevMotion : register(t0);
Texture2D<float4> Mda : register(t1);
Texture2D<float4> Color : register(t2);
Texture2D<float4> PrevMda : register(t3);
VKFMT("rgba8") RWTexture2D<float4> Out : register(u0);
Texture2D<float2> PrevEnv : register(t4);   // (the luma envelope where the pixel was: x its min, y its max)
VKFMT("rg8") RWTexture2D<float2> EnvOut : register(u1);

groupshared float gsLuma[100];  // (the group's 8 x 8 and a sample around: each encoded once, not by its 9 neighbours)

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 gid : SV_GroupID, uint li : SV_GroupIndex) {
    float e = encExposure();
    int2 p = int2(id), cs = int2(rs) - 1;
    // the colour's luma here and the 3x3's spread (the reactive test and the envelope - only when one is on)
    bool lumas = autoReactive > 0.0 || envGain > 0.0;
    if (lumas) {
        for (int i = (int)li; i < 100; i += 64) {
            int2 q = clamp(int2(gid * 8) - 1 + int2(i % 10, i / 10), int2(0, 0), cs);
            gsLuma[i] = dot(enc(Color.Load(int3(q, 0)).rgb, e), float3(0.25, 0.5, 0.25));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id >= (uint2)rs)) return;
    float lc = 0.0, lmn = 1.0, lmx = 0.0;
    if (lumas) {
        int2 o = p - int2(gid * 8);
        [unroll] for (int i = 0; i < 9; i++) {
            float l = gsLuma[(o.y + i / 3) * 10 + o.x + i % 3];
            lmn = min(lmn, l), lmx = max(lmx, l);
            if (i == 4) lc = l;
        }
    }
    float2 uv = (float2(id) + 0.5) * rsRcp;
    float4 mda4 = Mda.Load(int3(p, 0));
    float2 puv = uv - 0.5 * mda4.xy;
    bool off = any(puv < 0.0) || any(puv > 1.0);
    if (reset > 0.5 || off) {
        Out[id] = float4(0.0, 0.0, 0.0, lc);
        EnvOut[id] = float2(lc, lc);
        return;
    }
    // (the previous frame's motion where this pixel was: changed by over half a pixel more than its neighbours' -
    // another object's - less history, a narrower box. Against the neighbours: the camera speeding up or slowing down,
    // a hand on the mouse, changes every pixel's motion alike and cut the whole history; another object's edge changes it
    // here and not a little way off. The neighbours as far as the change itself (in output pixels, as when the upscale
    // did it), so a revealed strip's lie outside it)
    float4 pm = PrevMda.SampleLevel(linearClamp, puv, 0.0);
    float2 dlt = 0.5 * (mda4.xy - pm.xy) * os;
    float rad = clamp(length(dlt), 2.0, 16.0), pdif = 0.0;
    [unroll] for (int n = 0; n < 4; n++) {
        float2 nuv = uv + float2(n == 0 ? rad : n == 1 ? -rad : 0.0, n == 2 ? rad : n == 3 ? -rad : 0.0) * osRcp;
        float2 nm = Mda.SampleLevel(linearClamp, nuv, 0.0).xy;
        float2 npm = PrevMda.SampleLevel(linearClamp, nuv - 0.5 * nm, 0.0).xy;
        pdif = max(pdif, length(dlt - 0.5 * (nm - npm) * os));
    }
    float rj = smoothstep(0.5, 2.0, pdif);
    // (the disocclusion: where the pixel was, the previous frame's surfaces - even the farthest of the four
    // around it - nearer than this one, and moving otherwise than it (their motion: the previous frame's convert pass
    // took the nearest's): it was hidden behind something that moved away. A turn of the camera, a still scene, or a
    // thin blade the jitter hits one frame and misses the next keep the history - GSR 2's depth clip cut it at every
    // depth edge as soon as anything moved, the depth alone at every sub-pixel blade: the grass's noise)
    float4 pn = PrevMda.GatherAlpha(linearClamp, puv);
    float np = min(min(pn.x, pn.y), min(pn.z, pn.w)), nc = mda4.w;
    float nearer = np > 1e-7 ? smoothstep(0.03, 0.15, 1.0 - nc / np) : 0.0;
    float dz = nearer * smoothstep(0.25, 1.0, pdif);
    // (the derived reactive mask, where the game gives none: the luma here against the previous frame's where this pixel
    // was, beyond what the 3x3's own spread explains - the jitter's aliasing of an edge or a blade stays within it; a
    // particle, smoke, a transparency or a flash on a flat area does not. Its history then counts less: no trail behind
    // what moves without motion vectors of its own)
    float rv = 0.0;
    if (autoReactive > 0.0) {
        float lp = PrevMotion.SampleLevel(linearClamp, puv, 0.0).w;
        rv = autoReactive * saturate((abs(lc - lp) - 2.0 * (lmx - lmn) - 0.06) * 5.0);
    }
    // (the envelope: the range this pixel's luma has taken over the last frames - a decaying min / max, carried where
    // the pixel went. A static pixel's jittered samples bounce within it, however aliased; a luma outside it by more
    // than the tolerance is something new there - what moved with no motion vector of its own, arriving: the samples'
    // box, widened by the arrival itself, does not cut the history; this does, in the first frame)
    float2 env = float2(lc, lc);
    if (envGain > 0.0) {
        float2 pe = PrevEnv.SampleLevel(linearClamp, puv, 0.0);
        float ex = max(max(lc - pe.y, pe.x - lc), 0.0);
        float still = envRest > 0.5 ? 1.0 - saturate((length(0.5 * mda4.xy * os) - 0.5) / 1.5) : 1.0;
        float wdt = pe.y - pe.x;
        rv = max(rv, min(saturate((ex - envTol - envRel * wdt) * envGain), envCap) * still);
        env = float2(min(lc, pe.x + envDecay * wdt), max(lc, pe.y - envDecay * wdt));
    }
    EnvOut[id] = env;
    Out[id] = float4(rj, dz, rv, lc);
}
