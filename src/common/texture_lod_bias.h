// Pure texture mip-bias derivation for advanced.texture_lod_bias (VR and flat).
//
// In VR, Elite's render scale is <HMDRenderTargetMultiplier> in its newest
// Options\Graphics\*.fxcfg. In flat, the render fraction is <SSAAMultiplier>.
// Keeping the parser and derivation header-only lets config_test prove both
// profiles and missing-tag edge cases without spinning up a D3D11 device.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "temporal_mode.h"

namespace edvr {

// Reads advanced.texture_lod_bias using the active profile's temporal AA default
// ("auto" when temporal AA is enabled for that profile, else "0"). Single owner
// shared by device_hook.cpp and mode_watch.cpp so live-switch checks never drift.
template <typename Cfg>
inline std::string readTextureLodBiasKey(const Cfg& cfg, bool flatProfile) {
    const std::string temporalMode =
        flatProfile ? cfg.requestedTemporalMode() : cfg.getString("fix.temporal_aa", "off");
    const bool temporal = temporalModeEnabledFor(temporalMode, flatProfile);
    return cfg.getString("advanced.texture_lod_bias", temporal ? "auto" : "0");
}

struct TextureLodBiasResult {
    float bias = 0.0f;
    bool isAuto = false;
    bool autoMissing = false;
    float activeMult = 0.0f;
    char why[256] = {};
};

// Derives the sampler mip bias and explanatory log fragment from the configured
// biasKey ("auto" or a numeric string) and the multipliers read from .fxcfg.
inline TextureLodBiasResult resolveTextureLodBias(bool flatProfile, const std::string& biasKey,
                                                  float hmdMult, float ssaaMult,
                                                  const char* presetFile) {
    TextureLodBiasResult out;
    const char* fileLabel = (presetFile && presetFile[0]) ? presetFile : "its graphics preset";
    if (_stricmp(biasKey.c_str(), "auto") == 0) {
        const float active = flatProfile ? ssaaMult : hmdMult;
        if (active > 0.0f) {
            out.bias = std::log2(active);
            out.isAuto = true;
            out.activeMult = active;
            if (flatProfile) {
                std::snprintf(
                    out.why, sizeof(out.why),
                    "auto: Elite renders at %.3f of the output size "
                    "(SSAAMultiplier in %s; its HMDRenderTargetMultiplier is %.3f), and "
                    "log2 of that is the bias the mips want",
                    static_cast<double>(ssaaMult), fileLabel, static_cast<double>(hmdMult));
            } else {
                std::snprintf(
                    out.why, sizeof(out.why),
                    "auto: Elite renders at %.3f of the size the runtime recommends "
                    "(HMDRenderTargetMultiplier in %s; its SSAAMultiplier is %.3f), and "
                    "log2 of that is the bias the mips want",
                    static_cast<double>(hmdMult), fileLabel, static_cast<double>(ssaaMult));
            }
        } else {
            out.autoMissing = true;
        }
    } else {
        out.bias = static_cast<float>(std::atof(biasKey.c_str()));
        std::snprintf(out.why, sizeof(out.why), "advanced.texture_lod_bias = %s", biasKey.c_str());
    }
    if (!std::isfinite(out.bias)) out.bias = 0.0f;
    if (out.bias < -4.0f) out.bias = -4.0f;
    if (out.bias > 4.0f) out.bias = 4.0f;
    return out;
}

// Effective advanced.texture_lod_bias snapshot used by mode_watch.cpp's
// `kept until restart:` check on live VR <-> flat switches (#253).
// Labels contain no whitespace so tools/edup_log.py's KEPT_ITEM (\S+) parser
// matches them cleanly.
struct PatchedTextureLodBias {
    std::string label;
    float bias = 0.0f;
    bool resolved = false;
};

inline PatchedTextureLodBias effectiveTextureLodBias(bool flatProfile, const std::string& biasKey,
                                                     float hmdMult, float ssaaMult) {
    PatchedTextureLodBias out;
    const TextureLodBiasResult r = resolveTextureLodBias(flatProfile, biasKey, hmdMult, ssaaMult, nullptr);
    out.bias = r.bias;
    const bool isAuto = _stricmp(biasKey.c_str(), "auto") == 0;
    if (isAuto && r.autoMissing) {
        out.label = "auto";
        out.resolved = false;
        return out;
    }
    out.resolved = true;
    const double clean = std::fabs(static_cast<double>(r.bias)) < 0.0005 ? 0.0 : static_cast<double>(r.bias);
    char buf[32];
    if (isAuto) {
        std::snprintf(buf, sizeof(buf), "auto(%.2f)", clean);
        out.label = buf;
    } else {
        // Trim any surrounding whitespace so KEPT_ITEM (\S+) always parses the token.
        size_t s = 0;
        while (s < biasKey.size() && (biasKey[s] == ' ' || biasKey[s] == '\t')) ++s;
        size_t e = biasKey.size();
        while (e > s && (biasKey[e - 1] == ' ' || biasKey[e - 1] == '\t')) --e;
        std::string trimmed = biasKey.substr(s, e - s);
        if (trimmed.empty() || trimmed.find_first_of(" \t\r\n") != std::string::npos) {
            std::snprintf(buf, sizeof(buf), "%.2f", clean);
            out.label = buf;
        } else {
            out.label = std::move(trimmed);
        }
    }
    return out;
}

inline bool sameEffectiveTextureLodBias(const PatchedTextureLodBias& a, const PatchedTextureLodBias& b) {
    if (a.resolved && b.resolved) return std::fabs(a.bias - b.bias) < 0.005f;
    return a.resolved == b.resolved && a.label == b.label;
}

}  // namespace edvr
