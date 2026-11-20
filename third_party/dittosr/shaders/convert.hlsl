// DittoSR's convert pass, from Snapdragon GSR 2's (BSD 3-Clause, licenses/LICENSE-snapdragon-gsr.txt): the motion of
// the nearest of the 3x3. Render size. u0 (RGBA16F): xy = the motion in NDC (2 x (uv - the previous uv)), z unused,
// w = the nearest of the 3x3's nearness (1 - depth: reversed-Z's own value, ~ 1 / distance, precise in half floats where
// it is small) - the next frame's disocclusion test reads it where the pixel went. (GSR 2's depth clip, which once took
// z, gave way to the motion pass's disocclusion test.)
//   One group more, below the image's (the dispatch's last row, its first group): the fast settling - how much of the
// image changed against the history. 16 x 16 points of a grid over the render image, in a 4 x 4 of cells; at each,
// whether this frame's colour differs from the history where the point was by over 0.1 in luma; each cell's share, then
// their mean into u1's one texel, the upscale's. A scene change, a menu closing, a fade: most of the image at once - the
// upscale's reaction then goes further. (Once two passes of their own, a dispatch and a barrier each.)
// t0 = depth (1: far once un-inverted), t1 = the game's motion vectors, t2 = the colour, t3 = the previous history.
#include "common.hlsli"

Texture2D<float> InputDepth : register(t0);
Texture2D<float2> InputVelocity : register(t1);
Texture2D<float4> Color : register(t2);
Texture2D<float4> Hist : register(t3);
VKFMT("rgba16f") RWTexture2D<float4> Out : register(u0);
VKFMT("r32f") RWTexture2D<float> ChangeOut : register(u1);
VKFMT("r32f") RWTexture2D<float> ExposureOut : register(u2);  // (the auto exposure's 1 x 1)

float depthAt(int2 p, int2 ds) {
    float d = InputDepth.Load(int3(clamp(p, int2(0, 0), ds), 0));
    return depthInverted > 0.5 ? 1.0 - d : d;
}

// the motion at render pixel p, from the motion vectors' texel offset by best (in render pixels), in uv (uv - the
// previous uv)
float2 motionOf(int2 p, int2 best) {
    float2 texCoord = (float2(p) + 0.5) * rsRcp;
    int2 ms = int2(mvSize);
    int2 mp = clamp(int2(texCoord * mvSize), int2(0, 0), ms - 1);
    // (FSR: the previous uv = uv + mv x scale / size; ours: uv - the previous uv)
    // (the offset in the vectors' own texels: they may be at the output resolution)
    int2 mb = int2(round(float2(best) * mvSize * rsRcp));
    float2 mv = InputVelocity.Load(int3(clamp(mp + mb, int2(0, 0), ms - 1), 0));
    return -(mv * mvScale - jitCancel);
}

// the motion of the nearest of the 3x3 around render pixel p, in uv (uv - the previous uv); dmin: its depth
float2 motionAt(int2 p, out float dmin) {
    int2 ds = int2(rs) - 1, best = int2(0, 0);
    dmin = depthAt(p, ds);  // (the centre unless a neighbour is strictly nearer)
    for (int i = 0; i < 9; i++) {
        int2 q = int2(i % 3 - 1, i / 3 - 1);
        float dd = depthAt(p + q, ds);
        if (dd < dmin) { dmin = dd; best = q; }
    }
    return motionOf(p, best);
}

groupshared float gsHit[64], gsSeen[64];

void change(uint li) {
    // (64 threads: 4 per cell, 4 points each)
    float e = encExposure();
    int cell = (int)li / 4, part = (int)li % 4;
    int2 cp = int2(cell % 4, cell / 4), r = int2(rs);
    float n = 0.0, c = 0.0;
    for (int i = part * 4; i < part * 4 + 4; i++) {
        float2 g = (float2(cp * 4 + int2(i % 4, i / 4)) + 0.5) / 16.0;
        int2 q = int2(g * float2(r));
        float dmin;
        float2 puv = (float2(q) + 0.5) * rsRcp - motionAt(q, dmin);
        if (any(puv < 0.0) || any(puv > 1.0)) continue;
        float3 cur = enc(Color.Load(int3(q, 0)).rgb, e);
        float d = abs(dot(cur - Hist.SampleLevel(linearClamp, puv, 0.0).rgb, float3(0.25, 0.5, 0.25)));
        c += step(0.1, d); n += 1.0;
    }
    gsHit[li] = c, gsSeen[li] = n;
    GroupMemoryBarrierWithGroupSync();
    if (li != 0) return;
    float s = 0.0;
    for (int k = 0; k < 16; k++) {
        float cc = gsHit[k * 4] + gsHit[k * 4 + 1] + gsHit[k * 4 + 2] + gsHit[k * 4 + 3];
        float nn = gsSeen[k * 4] + gsSeen[k * 4 + 1] + gsSeen[k * 4 + 2] + gsSeen[k * 4 + 3];
        s += nn > 0.0 ? cc / nn : 0.0;
    }
    ChangeOut[uint2(0, 0)] = reset > 0.5 ? 0.0 : s / 16.0;
}

groupshared float gsLog[64];

// The auto exposure (the game set FSR's auto exposure flag and gave no exposure): FSR 3.1's, 1 / (9.6 x the scene's
// average luminance - geometric, of the colour without its pre-exposure), here over 32 x 32 points of the render image,
// eased over the frames (0.1 a frame in log2: it only sets where DittoSR's encoding compresses - undone on decoding,
// a lag changes no brightness). Without it a game's HDR far above 1 - a bright sky - was compressed so far that an edge
// against it, blended there, came out darker than the light it mixes: a dark outline against the sky (The Witcher 3).
// At most 1 (0.2.4's): a dark scene - a lit figure on a black menu - raised it thousands of times, the figure's light
// then all in the compression's shoulder, its history's slightest errors light and dark grain once decoded
void autoexp(uint li) {
    int2 r = int2(rs);
    float s = 0.0;
    for (int i = 0; i < 16; i++) {
        int k = (int)li * 16 + i;
        int2 q = int2((float2(k % 32, k / 32) + 0.5) / 32.0 * float2(r));
        float3 c = max(Color.Load(int3(q, 0)).rgb, 0.0) / max(preExposure, 1e-8);
        s += log2(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-5));
    }
    gsLog[li] = s;
    GroupMemoryBarrierWithGroupSync();
    if (li != 0) return;
    float t = 0.0;
    for (int j = 0; j < 64; j++) t += gsLog[j];
    float target = min(-log2(2.4) - t / 1024.0, 0.0);  // (log2 of the exposure: FSR's x 4 - a little more compression,
    // as DittoSR's encoding was tuned: with FSR's own, bright HDR trails behind wrong vectors weighed more (tiles80 3x -2 dB))
    float prev = Exposure.Load(int3(0, 0, 0));
    bool fresh = reset > 0.5 || !(prev > 1e-12 && prev < 1e12);
    ExposureOut[uint2(0, 0)] = exp2(fresh ? target : lerp(log2(prev), target, 0.1));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 gid : SV_GroupID, uint li : SV_GroupIndex) {
    if (gid.y == (uint(rs.y) + 7) / 8) {  // (the extra row: its first group the change, its second the exposure)
        if (gid.x == 0) change(li);
        else if (gid.x == 1 && autoExposure > 0.5) autoexp(li);
        return;
    }
    if (any(id >= (uint2)rs)) return;
    float2 dsz;
    InputDepth.GetDimensions(dsz.x, dsz.y);
    // (the gather between texels id - 1 and id, in the depth texture's own uv: it may be larger than the render size)
    float2 gatherCoord = float2(id) / dsz, step2 = 2.0 / dsz;
    float4 btmLeft = InputDepth.GatherRed(linearClamp, gatherCoord);
    float4 btmRight = InputDepth.GatherRed(linearClamp, gatherCoord + float2(step2.x, 0.0));
    float4 topLeft = InputDepth.GatherRed(linearClamp, gatherCoord + float2(0.0, step2.y));
    float4 topRight = InputDepth.GatherRed(linearClamp, gatherCoord + step2);
    if (depthInverted > 0.5) btmLeft = 1.0 - btmLeft, btmRight = 1.0 - btmRight, topLeft = 1.0 - topLeft, topRight = 1.0 - topRight;
    // (the nearest of the 3x3: a moving silhouette's own motion over the background around it - its depths from the
    // gathers above, which cover the 4 x 4 from id - 1 (a gather's x, y, z, w: (0, 1), (1, 1), (1, 0), (0, 0)); at the
    // image's edges, where the gathers clamp to the depth texture's own size, its own loads as before)
    float dmin;
    float2 v;
    [branch] if (all(id >= 1) && all(id + 1 < (uint2)rs)) {
        float d3[9] = {btmLeft.w, btmLeft.z, btmRight.w, btmLeft.x, btmLeft.y, btmRight.x, topLeft.w, topLeft.z, topRight.w};
        int2 best = int2(0, 0);
        dmin = d3[4];
        [unroll] for (int i = 0; i < 9; i++)
            if (d3[i] < dmin) { dmin = d3[i]; best = int2(i % 3 - 1, i / 3 - 1); }
        v = motionOf(int2(id), best);
    } else {
        v = motionAt(int2(id), dmin);
    }
    Out[id] = float4(2.0 * v, 0.0, 1.0 - dmin);
}
