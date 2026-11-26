// Elite's graphics preset inputs for sampler mip bias at launch and profile changes.
#pragma once

#include "../common/config.h"
#include "device_hook.h"
#include "terrain_checkerboard_reader.h"

#include <cstdio>
#include <cstdlib>

namespace edvr {

struct TextureLodBiasPreset {
    float hmd = 0.0f;
    float ssaa = 0.0f;
    char source[96] = {};
};

inline TextureLodBiasPreset readTextureLodBiasPreset(bool flat) {
    TextureLodBiasPreset preset;
    if (!flat) {
        (void)deviceHookFxcfgMultipliers(&preset.hmd, &preset.ssaa, preset.source, sizeof(preset.source));
        return preset;
    }

    tcn::Reading reading;
    if (tcn::readPresetTag(flatEliteGraphicsFolder(), executableDirectory(), "SSAAMultiplier", &reading)) {
        char* end = nullptr;
        const float value = std::strtof(reading.value, &end);
        if (*end == '\0' && value > 0.0f && value < 8.0f) preset.ssaa = value;
    }
    std::snprintf(preset.source, sizeof(preset.source), "%s", reading.source);
    return preset;
}

}  // namespace edvr
