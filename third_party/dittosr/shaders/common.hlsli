// DittoSR (D3D12 and Vulkan) - shared constants and the HDR encoding.
// Every pass reads the same root constants (dittosr.cpp DittoConstants, same order).

cbuffer C : register(b0) {
    float2 rs;            // render size
    float2 os;            // output size
    float2 rsRcp;
    float2 osRcp;
    float2 jit;           // FSR's jitterOffset, render pixels (the sample at p + 0.5 stands for p + 0.5 - jit)
    float ratio;          // output / render width
    float width;          // the narrow Gaussian's width (1.4)
    float alphaRest;      // a stable pixel's share of each new frame at rest (0.1)
    float conf;           // the confidence the narrow estimate is taken alone from (0.3)
    float reset;          // 1: this frame alone
    float gammaRest;      // the history clip width at rest (3)
    float react;          // the reaction to changes (1)
    float reactiveScale;  // the game's reactive mask: how far it raises the new frame's share (1)
    float stabStep;       // the stability level's step up each frame a sample lands near the pixel (0.34)
    float depthInverted;  // 1: depth 1 = near (reversed-Z)
    float2 mvScale;       // FSR motion vector -> this frame's uv - the previous one's (sign and size folded in)
    float2 mvSize;        // the motion vectors' resolution (render, or output with display-resolution vectors)
    float2 jitCancel;     // removed from the motion (uv) when the vectors carry the jitter
    float preExposure;
    float hdr;            // 1: linear HDR colour (Reinhard + sqrt around the history); 0: [0, 1] colour
    float exposureScale;  // the encoding's exposure multiplier (ini)
    float hasExposure;    // 1: t5 holds the game's 1x1 exposure
    float hasReactive;    // 1: t6 holds the game's reactive mask
    float debugView;      // 1: the motion as colour; 2: the new frame's share; 3: the stability level; 4: r = the clip (t - 1), g = cut, b = confidence
    float indicator;      // 1: DittoSR's face in the corner (DittoSR is running; common.hlsli indicatorAt)
    float motionGamma;    // the history clip width while moving (0.9 originally)
    float motionShare;    // the new frame's base share while moving (0.3)
    float motionMinMax;   // how far the box is held within the samples' min / max while moving (1)
    float antiShimmer;    // the anti-shimmer's strength (0: off)
    float sharpness;      // 0 - 1: at rest soft below 0.6, sharpened above; moving, sharpened by it (sharpen.hlsl)
    float changeScale;    // how far the share of the image that changed drives the reaction (1)
    float shareFar;       // the new frame's share x this where no sample landed close (0.6 originally)
    float shareNear;      // ... x this where one did (1.4: ANEAR)
    float encodePower;    // the encoding's power after Reinhard (0.5: sqrt; 1: Reinhard alone - blends closer to linear)
    float motionNear;     // shareNear while moving (from 4 output pixels a frame; 0: shareNear throughout)
    float splitClip;      // 1: the history clipped by its low frequencies only, its detail kept unless it moved far
                          // (0: the whole history clipped)
    float history10;      // 1: the history is RGB10A2 - its stores dithered by half a step
    float frameWeight;    // x the new frame's share (1): lower - more persistence, smoother and blurrier in motion; higher - more responsive
    float splitX;         // > 0: the comparison - the sharpening writes the output left of this column only (the game's
                          // own upscaler's on the right), a line between
    float autoReactive;   // the reactive mask DittoSR derives when the game gives none: its strength (0: off)
    float autoExposure;   // 1: the convert pass measures the exposure (the game asked for it, giving none): t5 the last
                          // frame's, u2 this one's
    float historySharpen; // the history's detail made up for its resampling blur, in motion (upscale.hlsl; 0: off)
    float fusedSharpen;   // > 0: the upscale sharpens and writes the output itself, the sharpening pass skipped
    float envTol;         // the envelope: how far outside its range a luma counts as new (0.04)
    float envDecay;       // ... the share of its width the range narrows by each frame (0.05)
    float envGain;        // ... the reactive mask per unit beyond the tolerance (0: off)
    float envRest;        // ... 1: only at rest (fading out from half an output pixel a frame)
    float rampLo, rampHi; // the speed (output pixels a frame) from which, and at which in full, the base share is the moving one
    float envRel;         // the envelope's tolerance more by this x its width
    float envCap;         // ... the most it raises the reactive mask to (0.5 and under: the stability is kept)
    float alphaSlow;      // > 0: the base share in slow motion (rampLo..rampHi), then on to the moving one by 8 px a frame
    float sharpenMode;    // 0: CAS; 1: RCAS (sharpen.hlsl)
};

// hf: the colour work's precision - half in the FP16 variant (DITTO_FP16, SM 6.2 with 16-bit types: twice the rate on
// RDNA and recent GPUs), float otherwise. Positions, weights and the moments stay float.
#if DITTO_FP16
typedef float16_t hf;
typedef float16_t3 hf3;
#else
typedef float hf;
typedef float3 hf3;
#endif

SamplerState linearClamp : register(s0);

// Vulkan (DITTO_VK, SPIR-V): every storage image with its format spelled out - writing one without needs a device
// feature the game may not have enabled. The passes that write the game's output (the upscale, the sharpening) are
// compiled once per output format (OUTFMT), the upscale once more per history format (HISTFMT: RGB10A2 or RGBA16F);
// the stability levels R8, or R8G8 with the anti-shimmer's state (the Vulkan upscale always has it: 0 turns it off).
// R8, R8G8 and RGB10A2 as storage images are Vulkan's extended formats: dittosr_vk.cpp checks the device has them.
#if DITTO_VK
#define VKFMT(f) [[vk::image_format(f)]]
#else
#define VKFMT(f)
#endif
#ifndef OUTFMT
#define OUTFMT "rgba16f"
#endif
#ifndef HISTFMT
#define HISTFMT "rgba16f"
#endif

Texture2D<float> Exposure : register(t5);

float max3(float3 c) { return max(c.r, max(c.g, c.b)); }

float encExposure() {
    float e = hasExposure > 0.5 ? Exposure.Load(int3(0, 0, 0)) : 1.0;
    return max(e * exposureScale, 1e-8) / max(preExposure, 1e-8);
}

// The game's colour -> DittoSR's domain: [0, 1], roughly perceptual (it was tuned for LDR gamma colour: its 1/255 box
// margin, its 0.1 luma change). HDR: x pre-exposure removed and exposed, Reinhard by the largest channel (reversible),
// then sqrt. NaN and negatives -> 0.
float3 enc(float3 c, float e) {
    c = float3(c.x > 0.0 ? c.x : 0.0, c.y > 0.0 ? c.y : 0.0, c.z > 0.0 ? c.z : 0.0);  // SM5: DXC's select() is not in D3DCompile
    if (hdr > 0.5) {
        c = min(c * e, 65000.0);
        c = c / (1.0 + max3(c));
    }
    [branch] if (encodePower == 0.5) return sqrt(saturate(c));  // (the default: sqrt, cheaper than pow - a branch, as
    return pow(saturate(c), encodePower);                       // a ?: would compute both)
}

float3 dec(float3 g, float e) {
    float3 t = saturate(g);
    [branch] if (encodePower == 0.5) t = t * t;
    else t = pow(t, 1.0 / encodePower);
    if (hdr > 0.5) {
        t = t / max(1.0 - max3(t), 1.0 / 4096.0);
        t = t / e;
    }
    return t;
}

float3 ycc(float3 c) { return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * (c.r - c.b), 0.5 * c.g - 0.25 * (c.r + c.b)); }
float3 rgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float mxc(float3 y) { return y.x + max(y.z, abs(y.y) - y.z); }  // (max(r, g, b) of a YCoCg colour)

// enc and ycc at hf precision: Reinhard in float (HDR values beyond half's range), the power and the rest in hf
#if DITTO_FP16
hf3 ench(float3 c, float e) {
    c = float3(c.x > 0.0 ? c.x : 0.0, c.y > 0.0 ? c.y : 0.0, c.z > 0.0 ? c.z : 0.0);  // SM5: DXC's select() is not in D3DCompile
    if (hdr > 0.5) {
        c = min(c * e, 65000.0);
        c = c / (1.0 + max3(c));
    }
    hf3 h = saturate((hf3)c);
    [branch] if (encodePower == 0.5) return sqrt(h);
    return pow(h, (hf)encodePower);
}
hf3 ycch(hf3 c) { return hf3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * (c.r - c.b), 0.5 * c.g - 0.25 * (c.r + c.b)); }
#else
#define ench enc
#define ycch ycc
#endif

// The indicator ([debug] indicator): DittoSR's icon in hard pixels - a pink rounded square, white eyes and smile - in
// the top-left corner, 12 x 12 cells of 1 pixel at 1080p, 2 at 1440p, 3 at 2160p. true: id is one of its pixels, c
// its colour in the game's output domain (linear when HDR, then / the exposure as the output is)
bool indicatorAt(uint2 id, float e, out float3 c) {
    c = 0.0;
    uint cell = max(1u, (uint)(os.y / 720.0));
    int2 p = int2(id / cell) - 1;  // (a cell's margin)
    if (any(p < 0) || any(p >= 12)) return false;
    static const uint shape[12] = {0x3FC, 0x7FE, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0x7FE, 0x3FC};
    static const uint white[12] = {0, 0, 0, 0, 0x090, 0, 0, 0x108, 0x0F0, 0, 0, 0};  // (the eyes one pixel, the smile within)
    uint bit = 1u << (uint)p.x;
    if (!(shape[p.y] & bit)) return false;
    // (the icon's gradient, F6BFE6 at the top to E89CD6 at the bottom)
    float3 pink = lerp(float3(0.965, 0.749, 0.902), float3(0.910, 0.612, 0.839), p.y / 11.0);
    c = (white[p.y] & bit) ? float3(1.0, 1.0, 1.0) : pink;
    if (hdr > 0.5) c = c * c * (0.3 + 0.7 * c);  // (about sRGB's decoding: linear)
    c /= e;
    return true;
}

// A history store: RGB10A2 rounds to 1/1023, and a stable pixel moves by 0.035 x the difference a frame - a difference
// under ~14 steps would never close. Half a step of noise (interleaved gradient noise, a new pattern each frame by the
// jitter) makes the rounding right on average.
float4 hstore(uint2 id, float3 g, float stab) {
    g = saturate(g);
    if (history10 > 0.5) {
        float2 p = float2(id) + jit * 97.0;
        float n = frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
        g = saturate(g + (n - 0.5) / 1023.0);
    }
    return float4(g, stab);
}
