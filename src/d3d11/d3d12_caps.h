// Shared D3D12 capability probe for XeSS-SR and FSR 4 (issue #256).
//
// Logs highest shader model, wave ops/lane counts, native 16-bit shader ops,
// and WaveMMA tier once per created D3D12 device so refusals and runtime
// filtering on Windows and Proton/vkd3d-proton are immediately diagnosable.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {

struct D3D12Caps {
    HRESULT shaderModelHr = E_POINTER;
    D3D_SHADER_MODEL highestShaderModel = D3D_SHADER_MODEL_5_1;

    HRESULT options1Hr = E_POINTER;
    bool waveOps = false;
    uint32_t waveLaneCountMin = 0;
    uint32_t waveLaneCountMax = 0;
    uint32_t totalLaneCount = 0;
    bool int64ShaderOps = false;

    HRESULT options4Hr = E_POINTER;
    bool native16BitShaderOps = false;

    HRESULT options9Hr = E_POINTER;
    D3D12_WAVE_MMA_TIER waveMmaTier = D3D12_WAVE_MMA_TIER_NOT_SUPPORTED;
};

inline const char* formatShaderModel(D3D_SHADER_MODEL sm, char (&buf)[16]) {
    // 0x00 is D3D_SHADER_MODEL_NONE; use numeric value so older Windows SDKs without the enumerator still compile.
    if (static_cast<unsigned>(sm) == 0u) {
        std::snprintf(buf, sizeof(buf), "none");
        return buf;
    }
    const unsigned major = (static_cast<unsigned>(sm) >> 4) & 0xFu;
    const unsigned minor = static_cast<unsigned>(sm) & 0xFu;
    std::snprintf(buf, sizeof(buf), "%u.%u", major, minor);
    return buf;
}

inline const char* formatWaveMmaTier(D3D12_WAVE_MMA_TIER tier, char (&buf)[16]) {
    switch (tier) {
        case D3D12_WAVE_MMA_TIER_NOT_SUPPORTED:
            std::snprintf(buf, sizeof(buf), "none");
            return buf;
        case D3D12_WAVE_MMA_TIER_1_0:
            std::snprintf(buf, sizeof(buf), "1.0");
            return buf;
        default:
            std::snprintf(buf, sizeof(buf), "tier(%u)", static_cast<unsigned>(tier));
            return buf;
    }
}

template <typename CheckFn>
inline D3D12Caps queryD3D12CapsWith(CheckFn&& checkFeatureSupport) {
    D3D12Caps caps;

    // Older Windows builds and vkd3d-proton runtimes return E_INVALIDARG if
    // asked about a D3D_SHADER_MODEL newer than they know; walk downward until
    // CheckFeatureSupport accepts a value or fails for another reason.
    // Use ABI-fixed hex values for 6.8/6.9 so earlier 10.0.22621/10.0.26100 SDK headers compile cleanly.
    static constexpr D3D_SHADER_MODEL kModels[] = {
        static_cast<D3D_SHADER_MODEL>(0x69), static_cast<D3D_SHADER_MODEL>(0x68),
        D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1,
    };
    for (D3D_SHADER_MODEL candidate : kModels) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{};
        sm.HighestShaderModel = candidate;
        const HRESULT hr = checkFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        caps.shaderModelHr = hr;
        if (SUCCEEDED(hr)) {
            caps.highestShaderModel = sm.HighestShaderModel;
            break;
        }
        if (hr != E_INVALIDARG) break;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
    caps.options1Hr = checkFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1));
    if (SUCCEEDED(caps.options1Hr)) {
        caps.waveOps = opt1.WaveOps != FALSE;
        caps.waveLaneCountMin = opt1.WaveLaneCountMin;
        caps.waveLaneCountMax = opt1.WaveLaneCountMax;
        caps.totalLaneCount = opt1.TotalLaneCount;
        caps.int64ShaderOps = opt1.Int64ShaderOps != FALSE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS4 opt4{};
    caps.options4Hr = checkFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &opt4, sizeof(opt4));
    if (SUCCEEDED(caps.options4Hr)) {
        caps.native16BitShaderOps = opt4.Native16BitShaderOpsSupported != FALSE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS9 opt9{};
    caps.options9Hr = checkFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &opt9, sizeof(opt9));
    if (SUCCEEDED(caps.options9Hr)) {
        caps.waveMmaTier = opt9.WaveMMATier;
    }

    return caps;
}

inline D3D12Caps queryD3D12Caps(ID3D12Device* device) {
    if (!device) return D3D12Caps{};
    return queryD3D12CapsWith([device](D3D12_FEATURE feature, void* data, UINT size) {
        return device->CheckFeatureSupport(feature, data, size);
    });
}

inline std::string formatD3D12Caps(const D3D12Caps& caps) {
    char smBuf[16]{};
    char mmaBuf[16]{};
    char part[160]{};
    std::string out;

    if (SUCCEEDED(caps.shaderModelHr)) {
        std::snprintf(part, sizeof(part), "sm=%s", formatShaderModel(caps.highestShaderModel, smBuf));
    } else {
        std::snprintf(part, sizeof(part), "sm=n/a(0x%08X)", static_cast<unsigned>(caps.shaderModelHr));
    }
    out += part;

    if (SUCCEEDED(caps.options1Hr)) {
        std::snprintf(part, sizeof(part), " waveOps=%u lanes=%u..%u totalLanes=%u int64=%u",
                      caps.waveOps ? 1u : 0u, caps.waveLaneCountMin, caps.waveLaneCountMax,
                      caps.totalLaneCount, caps.int64ShaderOps ? 1u : 0u);
    } else {
        std::snprintf(part, sizeof(part), " waveOps=n/a(0x%08X)", static_cast<unsigned>(caps.options1Hr));
    }
    out += part;

    if (SUCCEEDED(caps.options4Hr)) {
        std::snprintf(part, sizeof(part), " native16bit=%u", caps.native16BitShaderOps ? 1u : 0u);
    } else {
        std::snprintf(part, sizeof(part), " native16bit=n/a(0x%08X)", static_cast<unsigned>(caps.options4Hr));
    }
    out += part;

    if (SUCCEEDED(caps.options9Hr)) {
        std::snprintf(part, sizeof(part), " waveMMA=%s", formatWaveMmaTier(caps.waveMmaTier, mmaBuf));
    } else {
        std::snprintf(part, sizeof(part), " waveMMA=n/a(0x%08X)", static_cast<unsigned>(caps.options9Hr));
    }
    out += part;

    return out;
}

}  // namespace edvr
