// DittoSR's upscale. Output size.
// t0 = the previous history, t1 = the convert pass's (xy motion in NDC), t2 = the colour, t3 = the motion
// pass's (the motion checks, the derived reactive mask), t4 = the change's 1 x 1, t6 = the game's reactive mask
// (optional), t7 = the previous stability levels.
// u0 = the history (rgb in DittoSR's encoded domain), u1 = the game's output (the colour decoded back to the game's
// domain; fused or a debug view - else the sharpening writes it), u2 = the stability levels (R8 - the RGB10A2 history's
// alpha has 2 bits -, R8G8 with the anti-shimmer's state).
//   The current frame: the 3x3 render samples around the pixel, each where its jitter put it, weighted by a round
//   Gaussian of its distance in output pixels (narrow) and by a wider one in render pixels (smooth). (Once the narrow one
//   was stretched along the samples' edges, and widened while moving, a thin line's lock besides: on the Ally no visible
//   difference without them, 3-5 % faster - the one upscale since 0.3.0.) The history: Catmull-Rom at the
//   previous position, clipped toward the samples' mean in YCoCg to mean +- gamma sigma. The blend: a share of the new
//   frame by the samples' nearness, more where something changed (another object there, the depth standing out, much
//   of the image changed, the history well outside the box); a stable pixel at rest takes less of each new frame.
#include "common.hlsli"

Texture2D<float4> PrevOutput : register(t0);
Texture2D<float4> MotionDepthClip : register(t1);
Texture2D<float4> InputColor : register(t2);
Texture2D<float4> Motion : register(t3);
Texture2D<float> Change : register(t4);
Texture2D<float> Reactive : register(t6);
VKFMT(HISTFMT) RWTexture2D<float4> History : register(u0);
#if ANTI_SHIMMER
Texture2D<float2> PrevStab : register(t7);  // (x the stability level, y the anti-shimmer's running sign)
#else
Texture2D<float> PrevStab : register(t7);
#endif
VKFMT(OUTFMT) RWTexture2D<float4> Output : register(u1);
#if ANTI_SHIMMER
VKFMT("rg8") RWTexture2D<float2> StabOut : register(u2);
#else
VKFMT("r8") RWTexture2D<float> StabOut : register(u2);
#endif

float3 hist(float2 uv, out float3 low) {  // (Catmull-Rom in 5 bilinear taps; low: their plain mean, ~3 pixels wide)
    float2 sz = os, p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;
    float2 q0 = (c1 - 1.0) / sz, q3 = (c1 + 2.0) / sz, q12 = (c1 + w2 / w12) / sz;
    float3 a = PrevOutput.SampleLevel(linearClamp, float2(q12.x, q0.y), 0.0).rgb;
    float3 b = PrevOutput.SampleLevel(linearClamp, float2(q0.x, q12.y), 0.0).rgb;
    float3 c = PrevOutput.SampleLevel(linearClamp, q12, 0.0).rgb;
    float3 d = PrevOutput.SampleLevel(linearClamp, float2(q3.x, q12.y), 0.0).rgb;
    float3 g = PrevOutput.SampleLevel(linearClamp, float2(q12.x, q3.y), 0.0).rgb;
    low = 0.2 * (a + b + c + d + g);
    float3 r = a * w12.x * w0.y + b * w0.x * w12.y + c * w12.x * w12.y + d * w3.x * w12.y + g * w12.x * w3.y;
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return saturate(r / ws);
}

void put(uint2 id, float3 g, float2 stab, float e, float3 shown) {
    g = saturate(g);
    float4 hs = hstore(id, g, stab.x);
    History[id] = hs;
    // (fused: the output from the history as stored - RGB10A2 rounds it to 1/1023, as the sharpening pass reads it -
    // plus the sharpening's own term: a still pixel's frame-to-frame wobble below a step stays hidden as it did; the
    // unrounded value showed it, the shimmer at rest up 10 %)
    if (history10 > 0.5) shown = round(hs.rgb * 1023.0) / 1023.0 + (shown - g);
#if ANTI_SHIMMER
    StabOut[id] = stab;
#else
    StabOut[id] = stab.x;
#endif
    if (debugView < 0.5 && fusedSharpen <= 0.0) return;  // (sharpen.hlsl writes the output; a debug view or fused: here)
    float3 o = dec(saturate(shown), e);
    float3 ic;
    if (indicator > 0.5 && indicatorAt(id, e, ic)) o = ic;
    Output[id] = float4(o, 1.0);
}
void put(uint2 id, float3 g, float2 stab, float e) { put(id, g, stab, e, g); }

// The group's render samples: an 8 x 8 output tile reads at most 10 x 10 of them (8 / ratio and its 3x3's margin, at
// ratio 1 and above), each loaded and encoded once into groupshared memory - not by each of the 9 x ratio^2 output
// pixels around it (~20 at 1.5x, ~80 at 3x). One sample more on each side: the tile's bounds and a pixel's own 3x3
// round the same floor apart and can land a sample apart (seen at 3x, where q + jitter falls on whole samples). A
// pixel whose 3x3 still falls outside, or below ratio 1, its own loads.
// (the group: 16 x 8 output pixels - dittosr.cpp kUpGX, kUpGY: on the ROG Ally 0.16 ms less at 2x than 8 x 8, fewer
// samples of the tile's margin loaded per pixel; 32 x 8 and 16 x 16 slower)
#define GX 16
#define GY 8
#define TILE (GX + 4)   // (the tile's row pitch: the group's width at ratio 1 and its margin)
#define TILEH (GY + 4)
groupshared hf3 gsTile[TILE * TILEH];  // (the samples encoded, in YCoCg: converted once, not by each pixel's 3x3)

int2 sampleBase(uint2 p) {  // (the 3x3's centre sample, as main computes it)
    float2 uv = (float2(p) + 0.5) * osRcp, q = uv * rs;
    return int2(floor(q + jit));
}

hf3 sampleAt(int2 p, int2 cs, float e) {
    return ycch(ench(InputColor.Load(int3(clamp(p, int2(0, 0), cs), 0)).rgb, e));
}

// The disocclusion's fill: Catmull-Rom over the 4 x 4 jittered samples around q (from the tile), held within the
// nearest 2 x 2's range (no ringing) - where no history is left, an edge sharp from the first frame; the Gaussians'
// estimate is soft there, and blocky at 3x. false: the 4 x 4 not all in the tile.
bool spatial(float2 q, int2 t0, int2 tw, out float3 o) {
    float2 g = q + jit - 0.5;  // (sample p stands for p + 0.5 - jit: in sample index space)
    int2 p1 = int2(floor(g));
    float2 f = g - float2(p1);
    int2 a = p1 - 1 - t0;
    o = 0.0;
    if (any(a < 0) || any(a + 3 > tw - 1)) return false;
    float4 wx = float4(f.x * (-0.5 + f.x * (1.0 - 0.5 * f.x)), 1.0 + f.x * f.x * (-2.5 + 1.5 * f.x),
                       f.x * (0.5 + f.x * (2.0 - 1.5 * f.x)), f.x * f.x * (-0.5 + 0.5 * f.x));
    float4 wy = float4(f.y * (-0.5 + f.y * (1.0 - 0.5 * f.y)), 1.0 + f.y * f.y * (-2.5 + 1.5 * f.y),
                       f.y * (0.5 + f.y * (2.0 - 1.5 * f.y)), f.y * f.y * (-0.5 + 0.5 * f.y));
    float3 s = 0.0, mn = 1e4, mx = -1e4;
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int i = 0; i < 4; i++) {
            float3 c = (float3)gsTile[(a.y + j) * TILE + a.x + i];
            s += c * (wx[i] * wy[j]);
            if ((i == 1 || i == 2) && (j == 1 || j == 2)) { mn = min(mn, c); mx = max(mx, c); }
        }
    }
    o = clamp(s, mn, mx);
    return true;
}

[numthreads(GX, GY, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 gid : SV_GroupID, uint li : SV_GroupIndex) {
    float e = encExposure();
    int2 cs = int2(rs) - 1;
    uint2 g0 = gid * uint2(GX, GY);
    int2 t0 = sampleBase(g0) - 2, tw = sampleBase(min(g0 + uint2(GX - 1, GY - 1), (uint2)os - 1)) + 3 - t0;
    bool tiled = tw.x <= TILE && tw.y <= TILEH;
    if (tiled) {
        // (the row by a float reciprocal: an integer division by tw.x costs more than the loads)
        float rw = 1.0 / float(tw.x);
        for (int i = (int)li; i < tw.x * tw.y; i += GX * GY) {
            int y = int((float(i) + 0.5) * rw);
            int2 o = int2(i - y * tw.x, y);
            gsTile[o.y * TILE + o.x] = sampleAt(t0 + o, cs, e);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id >= (uint2)os)) return;
    float2 uv = (float2(id) + 0.5) * osRcp, q = uv * rs;
    float4 mda4 = MotionDepthClip.SampleLevel(linearClamp, uv, 0.0);
    float mlen = length(0.5 * mda4.xy * os);
    float mf = saturate((mlen - 0.5) / 3.5), wd = width > 0.0 ? width : 1.0;
    float ks = max(ratio, 1.0) / wd;
    int2 b0 = sampleBase(id);
    bool inTile = tiled && all(b0 - t0 >= 1) && all(b0 - t0 <= tw - 2);
    // (the samples' colour work at hf precision - half in the FP16 variant; positions and weights in float, the
    // moments in hf about the centre sample - no cancellation; the whole estimate in YCoCg, back to RGB once at the end)
    hf3 cc[9];
    [branch] if (inTile) {  // (one branch around the 9 reads, not one in each: 0.5 ms of 6 on a 2 CU RDNA 2)
        int o0 = (b0.y - t0.y - 1) * TILE + b0.x - t0.x - 1;
        [unroll] for (int i = 0; i < 9; i++) cc[i] = gsTile[o0 + (i / 3) * TILE + i % 3];
    } else {
        [unroll] for (int i = 0; i < 9; i++) cc[i] = sampleAt(b0 + int2(i % 3 - 1, i / 3 - 1), cs, e);
    }
    hf3 sg = 0.0, ymn = 1e4, ymx = -1e4, m1h = 0.0, m2h = 0.0;
    float wg = 0.0;
    const float L2E = 1.44269504;
    float2 d0 = float2(b0) + 0.5 - jit - q;
    float3 dx = d0.x + float3(-1.0, 0.0, 1.0), dy = d0.y + float3(-1.0, 0.0, 1.0);
    // the edge: the samples' luma gradient (Sobel); the narrow Gaussian stretched along it, narrow across it (r2 =
    // |d|^2 - 0.75 es dt^2, dt by steps from the centre's). 0.2.4 had it, 0.3.0 dropped it for speed; back with the
    // anti-shimmer at 0.8 it beat 0.3.1 on every 3x scene, PSNR and shimmer alike, +0.58 ms on a 2 CU RDNA 2)
    // (the samples' range and moments first: the edge's luma range is theirs)
    [unroll] for (int i = 0; i < 9; i++) {
        ymn = min(ymn, cc[i]); ymx = max(ymx, cc[i]);
        if (i != 4) { hf3 dc = cc[i] - cc[4]; m1h += dc; m2h += dc * dc; }
    }
    hf lmnh = ymn.x, lmxh = ymx.x;
    float gx = (float)((cc[2].x + (hf)2.0 * cc[5].x + cc[8].x) - (cc[0].x + (hf)2.0 * cc[3].x + cc[6].x));
    float gy = (float)((cc[6].x + (hf)2.0 * cc[7].x + cc[8].x) - (cc[0].x + (hf)2.0 * cc[1].x + cc[2].x));
    float g2 = gx * gx + gy * gy, rg = rsqrt(max(g2, 1e-10));
    float es = saturate(g2 * rg / (4.0 * (float)(lmxh - lmnh) + 1e-3));
    float2 tng = g2 > 1e-10 ? float2(-gy, gx) * rg : float2(0.0, 1.0);
    float kn = -2.26 * L2E * ks * ks, ke = 0.75 * es;
    float dt0 = dot(d0, tng);
    float3 m1 = (float3)m1h / 9.0, sig = sqrt(max((float3)m2h / 9.0 - m1 * m1, 0.0));
    m1 += (float3)cc[4];
    // (the smooth estimate - where no sample landed close, and with no history: the bilinear of the 4 samples around
    // the pixel (its 2 x 2 corner b0 or b0 - 1 on each axis). Once a round Gaussian over the 3 x 3: the same PSNR and
    // shimmer on the harness, 0.09 ms less on the ROG Ally at 1.5x and 2x)
    float2 gq = q + jit - 0.5, fb = gq - floor(gq);
    bool hx = floor(gq.x) >= float(b0.x), hy0 = floor(gq.y) >= float(b0.y);
    hf3 c00 = hy0 ? (hx ? cc[4] : cc[3]) : (hx ? cc[1] : cc[0]), c10 = hy0 ? (hx ? cc[5] : cc[4]) : (hx ? cc[2] : cc[1]);
    hf3 c01 = hy0 ? (hx ? cc[7] : cc[6]) : (hx ? cc[4] : cc[3]), c11 = hy0 ? (hx ? cc[8] : cc[7]) : (hx ? cc[5] : cc[4]);
    float3 cs2 = lerp(lerp((float3)c00, (float3)c10, fb.x), lerp((float3)c01, (float3)c11, fb.x), fb.y);
    // (the narrow Gaussian over those same 4 samples - the pixel's cell: the 3 x 3's other 5 are a render pixel or more
    // from the pixel, their weights under 3 % at 2x along an edge and ~0 at 3x. Over all 9 until 0.4.2: the harness'
    // battery the same at 2x and 3x, +- 0.01 dB at 1.5x; on the ROG Ally 0.125 ms less at 1080p 1.5x, none at 2x)
    {
        float ix = hx ? 0.0 : -1.0, iy = hy0 ? 0.0 : -1.0;
        float2 ddx = hx ? dx.yz : dx.xy, ddy = hy0 ? dy.yz : dy.xy;
        float2 tx = dt0 + tng.x * float2(ix, ix + 1.0), ty = tng.y * float2(iy, iy + 1.0);
        float4 dt4 = float4(tx.x + ty.x, tx.y + ty.x, tx.x + ty.y, tx.y + ty.y);
        float4 r2 = float4(ddx.x * ddx.x + ddy.x * ddy.x, ddx.y * ddx.y + ddy.x * ddy.x, ddx.x * ddx.x + ddy.y * ddy.y, ddx.y * ddx.y + ddy.y * ddy.y);
        float4 w4 = exp2(kn * (r2 - ke * dt4 * dt4));
        sg = c00 * (hf)w4.x + c10 * (hf)w4.y + c01 * (hf)w4.z + c11 * (hf)w4.w;
        wg = w4.x + w4.y + w4.z + w4.w;
    }
    float3 cg = (float3)sg / max(wg, 1e-6);
    float cf = saturate(wg);
    float3 cur = lerp(cs2, cg, smoothstep(0.05, conf > 0.0 ? conf : 0.6, cf));
    float dcur = cur.x - cs2.x;  // (fused sharpening: the new frame's detail - its narrow estimate over its smooth one)
    float2 puv = uv - 0.5 * mda4.xy;
    if (debugView > 0.5 && debugView < 1.5) { put(id, float3(saturate(abs(0.5 * mda4.xy * os) * 64.0), 0.0), float2(0.0, 0.5), 1.0); return; }
    bool off = any(puv < 0.0) || any(puv > 1.0);
    if (reset > 0.5 || off) {  // (no history: the disocclusion's fill, else the smooth estimate)
        float3 fill;
        put(id, rgb(inTile && spatial(q, t0, tw, fill) ? fill : cs2), float2(0.0, 0.5), e);
        return;
    }
    // the motion pass's, at render size: x = the motion's change against its neighbours' (rj), y = the disocclusion
    // (dz), z = the derived reactive mask
    float4 ax = Motion.SampleLevel(linearClamp, uv, 0.0);
    float rj = ax.x, dz = ax.y;
    // (where the history is lost - something revealed, another object's edge - the fill takes the place of the
    // Gaussians' estimate: sharp from the first frame)
    float lost = saturate(max(rj, dz) * 1.5 - 0.25);
    [branch] if (lost > 0.0 && inTile) {
        float3 fill;
        if (spatial(q, t0, tw, fill)) cur = lerp(cur, fill, lost);
    }
    float3 hl, hy = ycc(hist(puv, hl));
    hl = ycc(hl);
    // (the stability level where the pixel was)
#if ANTI_SHIMMER
    float2 sp2 = PrevStab.SampleLevel(linearClamp, puv, 0.0);
    float sp = sp2.x, up = sp2.y * 2.0 - 1.0;
#else
    float sp = PrevStab.SampleLevel(linearClamp, puv, 0.0);
#endif
    // (the share of the image that changed against its history - the change pass's one pixel)
    float gch = changeScale * smoothstep(0.2, 0.5, Change.Load(int3(0, 0, 0)));
    float rest = gammaRest > 0.0 ? gammaRest : 1.5;
    float gam = lerp(rest, motionGamma, saturate(mlen * 0.25)) * (1.0 - 0.5 * dz);
    gam *= 1.0 - 0.6 * rj;
    // (stable and at rest, a wider box - the jittered samples' own spread no longer cuts the history)
    gam *= 1.0 + 0.35 * sp * (1.0 - saturate(mlen));
    // (moving: the box mean +- gamma sigma within the samples' own range - min / max, as far as the pixel moves)
    float3 blo = m1 - gam * sig, bhi = m1 + gam * sig;
    float mm = motionMinMax * saturate(mlen * 0.5);
    blo = lerp(blo, max(blo, (float3)ymn), mm); bhi = lerp(bhi, min(bhi, (float3)ymx), mm);
    float3 bc = 0.5 * (blo + bhi), ext3 = 0.5 * (bhi - blo) + 1.0 / 255.0;
    // (splitClip: the box against the history's low frequencies - the jittered samples' aliasing of fine detail no
    // longer counts as a change - the clip moving those alone; the detail above them kept, fading as the low ones fall
    // far out: a real change, the detail stale as well. Else the whole history clipped, in motion toward one aliased frame)
    float3 hc = splitClip > 0.5 ? hl : hy;
    float3 dir = hc - bc;
    float3 ad = abs(dir) / ext3;
    float t = max(ad.x, max(ad.y, ad.z));
    float3 hcc = t > 1.0 ? bc + dir / t : hc;
    // (historySharpen: that detail a little stronger as far as the history was resampled off whole pixels, 4 f (1 - f) -
    // the axes' mean, nothing at rest or on whole-pixel pans. Catmull-Rom there softens it a little (half a pixel off:
    // detail 4 pixels wide x 0.88), and in motion that compounds frame after frame: moving detail went soft however
    // steady. x 1.03 up to 2x, 1.05 from 3x (constants.h), the extra bounded: beyond, the tiles' zone plate drifted; the
    // taps' own detail sharpened in hist(), bounded by their range, did as well but cost 0.3 ms on a 2 CU RDNA 2 - its
    // registers)
    float2 rf = frac(puv * os - 0.5), rl = 4.0 * rf * (1.0 - rf);
    float dk = 1.0 + historySharpen * 0.5 * (rl.x + rl.y);
    // (the extra held within the samples' range - past it, it grew into the tiles' zone plate over the frames)
    float3 dtl = (hy - hl) * (1.0 - smoothstep(1.0, 2.5, t)), bse = hcc + dtl;
    float3 lo = min(bse, min(hcc, (float3)ymn)), hi = max(bse, max(hcc, (float3)ymx));
    hy = splitClip > 0.5 ? clamp(bse + (dk - 1.0) * dtl, lo, hi) : hcc;
    float dhis = hy.x - hcc.x;  // (fused sharpening: the history's detail over its low frequencies)
    // (above 2x - hr 0 to 1 at 3x - moving, more of the new frame: its jittered samples are what carries a moving
    // object's detail, the history resampled every frame softening it - at 3x a moving object matched FSR's at 2.3x,
    // and the grass still shimmered half as much as FSR's)
    float hr = saturate(ratio - 2.0);
    float bRest = lerp(0.2, alphaRest > 0.0 ? alphaRest : 0.035, sp), bMove = motionShare * (1.0 + 0.67 * hr);
    float rmp = saturate((mlen - rampLo) / max(rampHi - rampLo, 1e-3));
    float base = alphaSlow > 0.0 ? lerp(lerp(bRest, max(bRest, alphaSlow), rmp), bMove, saturate((mlen - rampHi) / max(8.0 - rampHi, 1e-3)))
                                 : lerp(bRest, bMove, rmp);
    // (moving, less contrast between the pixels a sample landed near and the rest: each frame's own pattern of them
    // shimmers - at rest it is what resolves the detail)
    float near = motionNear > 0.0 ? lerp(shareNear, motionNear, mf) : shareNear;
    float landw = smoothstep(0.05, 0.8, cf);
    float a = base * lerp(shareFar, near, landw);
    // (frame_weight: the user's persistence - the reactions below still take over where something changed. Above 1, in
    // full moving and a quarter of its excess at rest: there the jittered samples it lets in showed as a grain - a
    // sharpening at rest with the sharpening off)
    float fwRest = frameWeight > 1.0 ? 1.0 + 0.25 * (frameWeight - 1.0) : frameWeight;
    a = saturate(a * lerp(fwRest, frameWeight, mf));
    // (anti_shimmer: the sign of the new frame's difference from the history, its running mean where the pixel was
    // (updated as samples land near it). Alternating - the samples scattered around the history: aliasing - less of the
    // new frame; consistent - the history lagging behind a change - as it is; none with the history outside the box
    // (a change, not aliasing: a moving shadow's edge keeps up))
#if ANTI_SHIMMER
    float sIn = clamp((cur.x - hy.x) * 250.0, -1.0, 1.0);
    float un = lerp(up, sIn, 0.5 * landw);
    a *= 1.0 - antiShimmer * (1.0 - abs(un)) * sp * saturate(2.0 - 2.0 * t);
#endif
    a = lerp(a, 0.6, dz);
    // the reaction: the history outside the samples' range by t - something changed - the new frame by it
    // (with much of the image changed the reaction goes further: the new frame up to 0.95)
    a = lerp(a, lerp(0.5, 0.95, gch), saturate((t - 1.0) * react));
    a = lerp(a, max(a, 0.5), gch);
    a = lerp(a, max(a, 0.6), rj);
    // (the game's reactive mask: particles, transparencies without motion of their own - more of the new frame)
    // (none from the game: the motion pass's derived one - the luma changed beyond what the jitter explains)
    float rv = 0.0;
    if (hasReactive > 0.5) rv = max(saturate(Reactive.SampleLevel(linearClamp, uv, 0.0) * reactiveScale), ax.z);  // (ax.z: the envelope's alone then)
    else rv = ax.z;
    a = max(a, rv);
    // (the stability for the next frame: 0 where the history was cut, else one step up. Above 2x at rest, a step only
    // when a sample landed near the pixel: at 3x one does every ~9 frames, and a step each frame had the pixel stable -
    // taking little of each new sample - before its first ones came; 30.8 to 31.9 dB on still grass at 3x, as a pixel
    // after a cut converges)
    bool cut = t > 1.25 || rj > 0.5 || dz > 0.5 || gch > 0.5 || rv > 0.5;
    float3 res = rgb(lerp(hy, cur, saturate(a)));
    // (very bright: the box, the clip and the fill are per YCoCg component - a saturated bright colour (an orange
    // light strip) at their corners came out with a channel above every sample's, near 1 encoded: decoded many times the
    // light, a flash the bloom spreads (Cyberpunk's club light bars flashed in bursts, the camera swaying). Each channel
    // no higher than the brightest sample's brightest one - the samples read again from the tile, not kept in registers.
    // It replaces 0.3.0's flash fix - the detail held within the samples' range where bright - and history_sharpen off
    // there: without them the harness' bright sky sparkled less (25.5 -> 21.2 at 2x), 0.08 ms less on a 2 CU RDNA 2)
    float rb = max(res.r, max(res.g, res.b));
    [branch] if (rb > 0.8 && inTile) {
        int o0 = (b0.y - t0.y - 1) * TILE + b0.x - t0.x - 1;
        float smx = 0.0;
        [unroll] for (int i = 0; i < 9; i++) smx = max(smx, mxc((float3)gsTile[o0 + (i / 3) * TILE + i % 3]));
        res = lerp(res, min(res, smx), smoothstep(0.8, 0.9, rb));
    }
    bool landed = hr <= 0.0 || cf > 0.15 * hr || mlen > 1.0;
    float stab = cut ? 0.0 : min(sp + (landed ? stabStep : 0.0), 1.0);
#if ANTI_SHIMMER
    float2 stab2 = float2(stab, 0.5 + 0.5 * (cut ? sIn : un));
#else
    float2 stab2 = float2(stab, 0.5);
#endif
    if (debugView > 1.5) {  // (the history as usual, the output a view of the decision)
        float3 v = debugView < 2.5 ? saturate(a).xxx : debugView < 3.5 ? sp.xxx : float3(saturate(t - 1.0), cut ? 1.0 : 0.0, cf);
        History[id] = hstore(id, res, stab);
        StabOut[id] = stab2;
        Output[id] = float4(v, 1.0);
        return;
    }
    // the fused sharpening (fusedSharpen > 0): the output, not the history - the blend's detail (the history's over
    // its low frequencies, the new frame's narrow estimate over its smooth one, by the frame's share) added to the luma
    // by sharpen.hlsl's strength (at rest by the sharpness and the stability, moving 0.1 x sharpness), within the
    // samples' luma range and a quarter beyond moving; not on very bright pixels (the decoding blows a little up)
    float3 shown = res;
    if (fusedSharpen > 0.0) {
        float sh = saturate(sharpness);
        float boost = 1.0 + 0.3 * saturate((ratio - 1.5) / 1.5) + 0.3 * saturate((2160.0 - os.y) / 1080.0);
        float kr = sh < 0.6 ? 0.25 * (sh - 0.6) : min(0.125 * boost, 0.2) * (sh - 0.6) / 0.4;
        float km = 0.1 * sh * (1.0 + 1.25 * hr);
        float mv = saturate(mlen * 0.5);
        float kk = min(lerp(kr > 0.0 ? kr * (0.5 + 0.5 * stab) : kr, km, mv), 0.2);
        float g = fusedSharpen * (0.8 - 0.5 * hr) * 4.0 * kk / (1.0 - 4.0 * kk);  // (0.8 up to 2x, 0.3 at 3x: the pass's sharpness)
        float yb = 0.25 * res.r + 0.5 * res.g + 0.25 * res.b;
        float mrg = 0.25 * (1.0 - hr) * (ymx.x - ymn.x) * mv;
        float yt = clamp(yb + g * lerp(dhis, dcur, saturate(a)), min(yb, ymn.x - mrg), max(yb, ymx.x + mrg));
        shown = res + (yt - yb) * (1.0 - smoothstep(0.8, 0.9, max(res.r, max(res.g, res.b))));
    }
    put(id, res, stab2, e, shown);
}
