// DittoSR 0.5.0's per-frame constants, packed as a Direct3D 11 constant buffer.
// The numbers are Vancete/DittoSR v0.5.0 src/constants.h (ditto_fill_constants)
// and the shipped DittoSettings defaults in src/dittosr.h. Root constants on
// DirectX 12 are packed tight; a cbuffer is not, so this struct follows HLSL's
// 16-byte register packing of shaders/common.hlsli rather than DittoConstants.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if !defined(_MSC_VER)
#include <strings.h>
#endif

namespace edvr {

struct DittoCb {
    float rs[2], os[2];
    float rsRcp[2], osRcp[2];
    float jit[2], ratio, width;
    float alphaRest, conf, reset, gammaRest;
    float react, reactiveScale, stabStep, depthInverted;
    float mvScale[2], mvSize[2];
    float jitCancel[2], preExposure, hdr;
    float exposureScale, hasExposure, hasReactive, debugView;
    float indicator, motionGamma, motionShare, motionMinMax;
    float antiShimmer, sharpness, changeScale, shareFar;
    float shareNear, encodePower, motionNear, splitClip;
    float history10, frameWeight, splitX, autoReactive;
    float autoExposure, historySharpen, fusedSharpen, envTol;
    float envDecay, envGain, envRest, rampLo;
    float rampHi, envRel, envCap, alphaSlow;
    float sharpenMode, pad0, pad1, pad2;
};
static_assert(sizeof(DittoCb) == 256, "DittoSR's cbuffer is 16 registers");
static_assert(offsetof(DittoCb, mvScale) == 80, "mvScale register");
static_assert(offsetof(DittoCb, sharpenMode) == 240, "sharpenMode register");

// What one evaluation asks for. Motion vectors are render pixels, current to
// previous, the same vectors FSR is given (fsr3_engine.cpp's scale of 1).
// DittoSR then wants previous uv = uv + mv / render size, so mvScale is
// 1/render size (dittosr.cpp ditto_frame_from_ffx, motionVectorScale / mvRes).
// jitteredMv stays false: ED-UP's vectors exclude the raster phase
// (hdr_backend_flags.h).
struct DittoEval {
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    float jitX = 0, jitY = 0;
    bool reset = false;
    bool hdr = false;
    bool depthInverted = true;
    bool hasReactive = false;
    bool autoExposure = false;
    float sharpness = 0.6f;     // 0 soft, 0.6 as reconstructed, 1 sharpened
    float frameWeight = 2.0f;   // 0.25..4; lower holds history longer
    float antiShimmer = -1.0f;  // below 0: the ratio's own strength; 0 off; otherwise 0.1..0.9
    int sharpenMode = 0;        // 0 CAS, 1 RCAS
};

inline float dittoClamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

inline bool dittoSameText(const char* a, const char* b) {
    if (!a || !b) return a == b;
#if defined(_MSC_VER)
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

// auto, empty, or anything that is not a number: DittoSR's ratio curve (-1). off: 0. A number is clamped to 0..0.9.
inline float dittoAntiShimmerFromText(const char* text) {
    if (!text || !text[0] || dittoSameText(text, "auto")) return -1.0f;
    if (dittoSameText(text, "off")) return 0.0f;
    char* end = nullptr;
    const float asked = std::strtof(text, &end);
    if (!end || end == text || *end != 0) return -1.0f;
    return dittoClamp(asked, 0.0f, 0.9f);
}

// rcas is 1. cas, and any other word, is 0.
inline int dittoSharpenModeFromText(const char* text) { return text && dittoSameText(text, "rcas") ? 1 : 0; }

inline DittoCb dittoFillConstants(const DittoEval& in) {
    DittoCb k{};
    const float rw = static_cast<float>(in.rw), rh = static_cast<float>(in.rh);
    const float ow = static_cast<float>(in.ow), oh = static_cast<float>(in.oh);
    k.rs[0] = rw;
    k.rs[1] = rh;
    k.os[0] = ow;
    k.os[1] = oh;
    k.rsRcp[0] = 1.0f / rw;
    k.rsRcp[1] = 1.0f / rh;
    k.osRcp[0] = 1.0f / ow;
    k.osRcp[1] = 1.0f / oh;
    k.jit[0] = in.jitX;
    k.jit[1] = in.jitY;
    k.ratio = ow / rw;
    const float lo = k.ratio <= 1.5f ? 0.0f : k.ratio >= 2.0f ? 1.0f : (k.ratio - 1.5f) / 0.5f;
    k.width = 0.8f * (0.875f + 0.125f * lo);
    k.alphaRest = 0.1f;
    k.conf = 0.05f;
    k.reset = in.reset ? 1.0f : 0.0f;
    k.gammaRest = 3.0f;
    k.react = 1.0f;
    k.reactiveScale = 1.0f;
    k.stabStep = 0.34f;
    k.depthInverted = in.depthInverted ? 1.0f : 0.0f;
    k.mvScale[0] = 1.0f / rw;
    k.mvScale[1] = 1.0f / rh;
    k.mvSize[0] = rw;
    k.mvSize[1] = rh;
    k.preExposure = 1.0f;
    k.hdr = in.hdr ? 1.0f : 0.0f;
    k.exposureScale = 1.0f;
    k.hasExposure = in.autoExposure ? 1.0f : 0.0f;
    k.hasReactive = in.hasReactive ? 1.0f : 0.0f;
    k.motionGamma = 1.5f;
    k.motionShare = 0.15f;
    k.motionMinMax = 1.0f;
    k.sharpness = dittoClamp(in.sharpness, 0.0f, 1.0f);
    k.changeScale = 1.0f;
    k.shareFar = 0.2f;
    k.shareNear = 3.0f;
    k.encodePower = 0.5f;
    k.motionNear = 1.5f;
    k.splitClip = 1.0f;
    k.history10 = 0.0f;  // RGBA16F history: Direct3D 11 has no dependable RGB10A2 UAV
    k.frameWeight = dittoClamp(in.frameWeight, 0.25f, 4.0f);
    k.autoReactive = in.hasReactive ? 0.0f : 0.5f;
    const float hr = k.ratio <= 2.0f ? 0.0f : k.ratio >= 3.0f ? 1.0f : k.ratio - 2.0f;
    k.historySharpen = 0.03f + 0.02f * hr;
    // Below zero is DittoSR's Auto: 0.6 up to 2x, easing to 0.45 by 3x. A set value is used as given, and never above 0.9.
    float as = in.antiShimmer >= 0.0f ? in.antiShimmer : 0.6f - 0.15f * hr;
    if (as > 0.9f) as = 0.9f;
    if (as < 0.0f) as = 0.0f;
    k.antiShimmer = as;
    k.fusedSharpen = in.sharpenMode == 0 && k.ratio <= 2.55f ? 1.0f : 0.0f;
    k.rampLo = 0.25f;
    k.rampHi = 1.0f;
    k.envRel = 0.25f;
    k.alphaSlow = 0.15f;
    k.envCap = 0.5f * (1.0f - 0.5f * hr);
    k.shareFar *= 1.0f - 0.5f * hr;
    k.envTol = 0.03f;
    k.envDecay = 0.05f;
    k.envGain = 40.0f;
    k.envRest = 1.0f;
    k.sharpenMode = in.sharpenMode == 1 ? 1.0f : 0.0f;
    k.autoExposure = in.autoExposure ? 1.0f : 0.0f;
    return k;
}

}  // namespace edvr
