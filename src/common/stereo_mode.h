// The game's 3D mode, read from Settings.xml's text. Pure and free of any Windows API, so the launch decision (install_scope.h), the
// Supersampling hold (src\d3d11\vr_ssaa_hold_math.h) and the rigs that drive them share one parser.
#pragma once

#include <cstring>
#include <string>

namespace edvr {

// The value of <StereoscopicMode> in Settings.xml's text. True with *mode = N when the tag holds a whole number from 0 to 6 (the
// game clamps it to that range), with whitespace allowed around it and the complete </StereoscopicMode> closing tag after it; false
// when the tag is absent or holds anything else, a truncated or mismatched closing tag included.
inline bool parseStereoscopicMode(const char* xml, int* mode) {
    static const char kTag[] = "<StereoscopicMode>";
    const char* at = xml ? std::strstr(xml, kTag) : nullptr;
    if (!at) return false;
    const char* p = at + (sizeof(kTag) - 1);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 6) return false;
        ++p;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    // The closing tag must follow in full. Settings.xml is read while the game may be rewriting it, so a cut-off "0<" or "0</Stereo" is
    // not a value; strncmp stops at the end of the text, so a short read cannot run past it.
    static const char kClose[] = "</StereoscopicMode>";
    if (std::strncmp(p, kClose, sizeof(kClose) - 1) != 0) return false;
    *mode = v;
    return true;
}

// Elite Dangerous's in-game Options > Graphics > 3D <StereoscopicMode> values
// (matching the option token order in EliteDangerous64.exe):
//   0 = Off ($THREE_D_OFF, flat)
//   1 = Anaglyph ($THREE_D_ANAGLYPH, flat)
//   2 = Side by Side ($THREE_D_SIDE_BY_SIDE)
//   3 = HMD (Headphones) ($THREE_D_HMD_WITH_HEADPHONES)
//   4 = HMD (Speakers) ($THREE_D_HMD_WITH_SPEAKERS)
//   5 = HMD Cinema (Headphones) ($THREE_D_HMD_CINEMA_WITH_HEADPHONES)
//   6 = HMD Cinema (Speakers) ($THREE_D_HMD_CINEMA_WITH_SPEAKERS)
inline constexpr int kStereoModeOff = 0;
inline constexpr int kStereoModeAnaglyph = 1;
inline constexpr int kStereoModeSideBySide = 2;
inline constexpr int kStereoModeHmdHeadphones = 3;
inline constexpr int kStereoModeHmdSpeakers = 4;
inline constexpr int kStereoModeHmdCinemaHeadphones = 5;
inline constexpr int kStereoModeHmdCinemaSpeakers = 6;

inline bool isVrStereoscopicMode(int mode) { return mode >= 2 && mode <= 6; }

inline const char* stereoscopicModeName(int mode) {
    switch (mode) {
        case 0: return "Off";
        case 1: return "Anaglyph";
        case 2: return "Side by Side";
        case 3: return "HMD (Headphones)";
        case 4: return "HMD (Speakers)";
        case 5: return "HMD Cinema (Headphones)";
        case 6: return "HMD Cinema (Speakers)";
        default: return "Unknown";
    }
}

enum class StereoWriteStatus {
    Unchanged,      // already matches the requested launch (flat: 0; VR: preferred mode 2..6)
    Updated,        // <StereoscopicMode> updated to "0" (flat) or preferred VR mode (2..6)
    TagAbsent,      // <StereoscopicMode> tag not present in XML (and could not be inserted)
    ParseFailed,    // <StereoscopicMode> tag present but value or closing tag is invalid
    NoHeadsetMode,  // VR launch requested while preferred VR 3D mode is Not Set (0)
};

// Prepares a minimal byte-preserving Settings.xml update for a launch (`vr` = true for VR, false for flat).
// - Flat: replaces only the digit text inside <StereoscopicMode> with "0" (Unchanged when already 0;
//   TagAbsent when no tag is present, which leaves the file untouched).
// - VR: requires `preferredVrMode` in 2..6; returns NoHeadsetMode (writing nothing) when `preferredVrMode`
//   is Not Set (0 / out of range). When set, replaces only the digit text inside <StereoscopicMode>
//   (Unchanged if it already matches), or inserts the tag before </GraphicsOptions> if absent.
// - A parse failure always writes nothing.
inline StereoWriteStatus rewriteStereoscopicMode(const std::string& xml, bool vr, int preferredVrMode,
                                                 std::string* outXml, int* existingMode = nullptr) {
    static const char kTag[] = "<StereoscopicMode>";
    static const char kClose[] = "</StereoscopicMode>";
    if (vr && !isVrStereoscopicMode(preferredVrMode)) return StereoWriteStatus::NoHeadsetMode;

    const size_t openPos = xml.find(kTag);
    if (openPos == std::string::npos) {
        if (vr) {
            static const char kRootClose[] = "</GraphicsOptions>";
            const size_t closeRoot = xml.find(kRootClose);
            if (closeRoot != std::string::npos) {
                const char* eol = xml.find("\r\n") != std::string::npos ? "\r\n" : "\n";
                const std::string insert = std::string("\t<StereoscopicMode>") +
                                           std::to_string(preferredVrMode) + "</StereoscopicMode>" + eol;
                if (outXml) *outXml = xml.substr(0, closeRoot) + insert + xml.substr(closeRoot);
                return StereoWriteStatus::Updated;
            }
        }
        return StereoWriteStatus::TagAbsent;
    }

    size_t p = openPos + (sizeof(kTag) - 1);
    while (p < xml.size() && (xml[p] == ' ' || xml[p] == '\t' || xml[p] == '\r' || xml[p] == '\n')) ++p;
    if (p >= xml.size() || xml[p] < '0' || xml[p] > '9') return StereoWriteStatus::ParseFailed;

    const size_t valStart = p;
    int v = 0;
    while (p < xml.size() && xml[p] >= '0' && xml[p] <= '9') {
        v = v * 10 + (xml[p] - '0');
        if (v > 6) return StereoWriteStatus::ParseFailed;
        ++p;
    }
    const size_t valEnd = p;
    while (p < xml.size() && (xml[p] == ' ' || xml[p] == '\t' || xml[p] == '\r' || xml[p] == '\n')) ++p;
    const size_t closeLen = sizeof(kClose) - 1;
    if (p + closeLen > xml.size() || xml.compare(p, closeLen, kClose) != 0) return StereoWriteStatus::ParseFailed;

    if (existingMode) *existingMode = v;
    if (vr) {
        if (v == preferredVrMode) return StereoWriteStatus::Unchanged;
        if (outXml) *outXml = xml.substr(0, valStart) + std::to_string(preferredVrMode) + xml.substr(valEnd);
        return StereoWriteStatus::Updated;
    }
    if (v == 0) return StereoWriteStatus::Unchanged;
    if (outXml) *outXml = xml.substr(0, valStart) + "0" + xml.substr(valEnd);
    return StereoWriteStatus::Updated;
}

}  // namespace edvr
