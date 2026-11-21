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

enum class StereoWriteStatus {
    Unchanged,      // already matches the requested launch (flat: 0; VR: 2..6)
    Updated,        // flat launch: inner digits of <StereoscopicMode> replaced with "0"
    TagAbsent,      // <StereoscopicMode> tag not present in XML
    ParseFailed,    // <StereoscopicMode> tag present but value or closing tag is invalid
    NoHeadsetMode,  // VR launch requested, but tag is 0 or 1 (no recorded headset mode)
};

// Prepares a minimal byte-preserving Settings.xml update for a launch (`vr` = true for VR, false for flat).
// - Flat: replaces only the digit text inside <StereoscopicMode> with "0" (Unchanged when already 0).
// - VR: leaves any existing headset mode 2..6 unchanged; refuses 0, 1, absent, or unparseable tags
//   without inventing a default.
inline StereoWriteStatus rewriteStereoscopicMode(const std::string& xml, bool vr, std::string* outXml,
                                                 int* existingMode = nullptr) {
    static const char kTag[] = "<StereoscopicMode>";
    static const char kClose[] = "</StereoscopicMode>";
    const size_t openPos = xml.find(kTag);
    if (openPos == std::string::npos) return StereoWriteStatus::TagAbsent;

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
        if (v >= 2 && v <= 6) return StereoWriteStatus::Unchanged;
        return StereoWriteStatus::NoHeadsetMode;
    }
    if (v == 0) return StereoWriteStatus::Unchanged;
    if (outXml) *outXml = xml.substr(0, valStart) + "0" + xml.substr(valEnd);
    return StereoWriteStatus::Updated;
}

}  // namespace edvr
