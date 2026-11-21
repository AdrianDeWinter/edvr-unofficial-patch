#include "settings.h"

#include <windows.h>

#include "detect.h"
#include "../common/flat_profile_keys.h"
#include "../common/iniedit.h"
#include "../common/pad_names.h"
#include "state.h"

namespace edvr::installer {
namespace {

#include "settings_schema.inc"  // generated: kSettings[]

// The backup folder for this session's first change, and the folder above it.
// (The write itself is iniedit's writeFileAtomic: this rewrites the WHOLE of
// somebody's edvr.ini on every toggle, and the game re-reads that file about
// once a second, so a truncate-then-write left a poll reading a 0-byte ini and
// dropping every setting to its compiled default until the next reload.)
bool ensureBackupDir(const std::wstring& path) {
    if (dirExists(path)) return true;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring parent = path.substr(0, slash);
        if (!dirExists(parent) && !CreateDirectoryW(parent.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return CreateDirectoryW(path.c_str(), nullptr) != 0 ||
           (GetLastError() == ERROR_ALREADY_EXISTS && dirExists(path));
}

}  // namespace

std::vector<Choice> splitChoices(const char* packed) {
    std::vector<Choice> out;
    if (!packed || !*packed) return out;
    std::string current;
    auto flush = [&]() {
        if (current.empty()) return;
        Choice choice;
        const size_t equals = current.find('=');
        if (equals == std::string::npos) {
            choice.value = current;
            choice.label = current;
        } else {
            choice.value = current.substr(0, equals);
            choice.label = current.substr(equals + 1);
        }
        out.push_back(choice);
        current.clear();
    };
    for (const char* p = packed; *p; ++p) {
        if (*p == '|')
            flush();
        else
            current += *p;
    }
    flush();
    return out;
}

namespace {

// A number as somebody would write it: no trailing zeros, no ".0" on a whole
// one, and never scientific notation for the small values these settings use.
std::string tidyNumber(double value, int decimals) {
    char buffer[64];
    sprintf_s(buffer, "%.*f", decimals, value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text.empty() ? "0" : text;
}

bool asNumber(const std::string& text, double* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    while (end && *end == ' ') ++end;
    if (end && *end != '\0' && *end != '%') return false;
    *out = value;
    return true;
}

// 0, 0.0 and 0.00 are one value, and a row that calls a file saying "0.70"
// different from a recommendation of "0.7" is wrong about the only thing it
// was asked to know.
bool sameValue(const SettingDef& def, const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (def.kind != SettingKind::Number) return false;
    double left = 0, right = 0;
    if (!asNumber(a, &left) || !asNumber(b, &right)) return false;
    const double difference = left > right ? left - right : right - left;
    return difference < 1e-9;
}

// The file's text, in the terms the window uses.
std::wstring showValue(const SettingDef& def, const std::string& fileValue) {
    if (!def.percent) return fromUtf8(fileValue);
    double value = 0;
    if (!asNumber(fileValue, &value)) return fromUtf8(fileValue);
    return fromUtf8(tidyNumber(value * 100.0, 1)) + L"%";
}

// Validates and canonicalises a hotkey value using the same grammar as
// src/common/hotkey.cpp (empty = unbound, optional CTRL/ALT/SHIFT modifiers +
// key name/character/0x##, GamePad_*, or 8-hex-digit device + Joy_*).
bool parseHotkeyText(const std::string& raw, std::string* out) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        out->clear();
        return true;
    }
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(b, e - b + 1);

    if (_strnicmp(s.c_str(), "GamePad_", 8) == 0) {
        const edvr::PadName* p = edvr::padNameFind(s.c_str());
        if (!p) return false;
        *out = p->elite;
        return true;
    }

    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    if (s.size() > 9 && s[8] == ':') {
        bool devHex = true;
        for (int i = 0; i < 8; ++i) {
            if (!isHex(s[i])) {
                devHex = false;
                break;
            }
        }
        if (devHex) {
            std::string dev = s.substr(0, 8);
            for (char& c : dev) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            std::string rest = s.substr(9);
            size_t rb = rest.find_first_not_of(" \t");
            if (rb == std::string::npos) return false;
            rest = rest.substr(rb);
            if (_strnicmp(rest.c_str(), "Joy_", 4) != 0) return false;
            const char* r = rest.c_str() + 4;
            if (_strnicmp(r, "POV", 3) == 0) {
                r += 3;
                if (*r < '1' || *r > '4') return false;
                const char hat = *r++;
                static const char* const kDirs[4] = {"Up", "Right", "Down", "Left"};
                for (const char* dir : kDirs) {
                    if (_stricmp(r, dir) == 0) {
                        *out = dev + ":Joy_POV" + std::string(1, hat) + dir;
                        return true;
                    }
                }
                return false;
            }
            if (*r < '1' || *r > '9') return false;
            int n = 0;
            for (const char* p = r; *p; ++p) {
                if (*p < '0' || *p > '9') return false;
                n = n * 10 + (*p - '0');
                if (n > 128) return false;
            }
            *out = dev + ":Joy_" + std::to_string(n);
            return true;
        }
    }

    bool ctrl = false, alt = false, shift = false;
    size_t pos = 0;
    while (true) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
        size_t w = pos;
        while (w < s.size() && isalpha(static_cast<unsigned char>(s[w]))) ++w;
        std::string word = s.substr(pos, w - pos);
        for (char& c : word) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        int mod = 0;
        if (word == "CTRL" || word == "CONTROL") mod = 1;
        else if (word == "ALT" || word == "MENU") mod = 2;
        else if (word == "SHIFT") mod = 3;
        if (!mod) break;
        size_t after = w;
        while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
        if (after >= s.size() || (s[after] != '+' && s[after] != '-')) break;
        if (mod == 1) ctrl = true;
        else if (mod == 2) alt = true;
        else shift = true;
        pos = after + 1;
    }
    size_t kb = s.find_first_not_of(" \t", pos);
    if (kb == std::string::npos) return false;
    std::string key = s.substr(kb);

    std::string canonical;
    if (key.size() >= 3 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        for (size_t i = 2; i < key.size(); ++i) {
            if (!isHex(key[i])) return false;
        }
        const long v = strtol(key.c_str(), nullptr, 16);
        if (v <= 0 || v > 0xFF) return false;
        char buf[8];
        sprintf_s(buf, "0x%02X", static_cast<unsigned>(v));
        canonical = buf;
    } else if (key.size() == 1 && key[0] > 0x20 && key[0] < 0x7F) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        canonical.assign(1, c);
    } else {
        static const char* const kKnownKeys[] = {
            "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
            "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
            "SCROLLLOCK", "SCROLL", "PAUSE", "BREAK", "NUMLOCK", "CAPSLOCK", "PRINTSCREEN",
            "INSERT", "INS", "DELETE", "DEL", "HOME", "END", "PAGEUP", "PGUP", "PAGEDOWN", "PGDN",
            "NUMPAD0", "NUMPAD1", "NUMPAD2", "NUMPAD3", "NUMPAD4",
            "NUMPAD5", "NUMPAD6", "NUMPAD7", "NUMPAD8", "NUMPAD9",
            "MULTIPLY", "DIVIDE", "ADD", "SUBTRACT", "DECIMAL", "NUMPADDOT",
            "RIGHT", "LEFT", "UP", "DOWN", "RIGHTARROW", "LEFTARROW", "UPARROW", "DOWNARROW",
            "SPACE", "TAB", "ENTER", "RETURN", "BACKSPACE", "ESCAPE", "ESC", "APPS", "MENU_KEY",
            "BACKSLASH", "SLASH", "LEFTBRACKET", "RIGHTBRACKET", "SEMICOLON", "APOSTROPHE",
            "QUOTE", "COMMA", "PERIOD", "DOT", "GRAVE", "BACKTICK", "TILDE", "MINUS", "DASH",
            "EQUALS", "PLUS", "HASH", "POUND",
        };
        for (const char* name : kKnownKeys) {
            if (_stricmp(key.c_str(), name) == 0) {
                canonical = name;
                break;
            }
        }
        if (canonical.empty()) return false;
    }

    std::string prefix;
    if (ctrl) prefix += "CTRL+";
    if (alt) prefix += "ALT+";
    if (shift) prefix += "SHIFT+";
    *out = prefix + canonical;
    return true;
}

}  // namespace

std::wstring SettingRow::shown() const {
    return def ? showValue(*def, value) : std::wstring();
}

std::wstring SettingRow::shownRecommended() const {
    return def ? showValue(*def, def->recommended) : std::wstring();
}

bool SettingRow::parseTyped(const std::wstring& typed, std::string* fileValue) const {
    if (!def) return false;
    const std::string text = toUtf8(typed);
    if (def->kind == SettingKind::Text) {
        if (def->section && strcmp(def->section, "hotkey") == 0) {
            return parseHotkeyText(text, fileValue);
        }
        *fileValue = text;
        return true;
    }
    double value = 0;
    if (!asNumber(text, &value)) return false;
    // "30", "30%" and "0.3" all mean the same thing on a percentage setting.
    // The last of those is what the file already holds, so a value that is
    // already a fraction is left alone rather than turned into 0.003.
    if (def->percent) {
        const bool typedAsFraction = text.find('%') == std::string::npos && value > 0.0 &&
                                     value <= 1.0 && text.find('.') != std::string::npos;
        if (!typedAsFraction) value /= 100.0;
    }
    *fileValue = tidyNumber(value, def->percent ? 4 : (def->precision > 0 ? 4 : 0));
    return true;
}

std::wstring SettingRow::helper() const {
    if (!def || def->kind == SettingKind::Toggle) return std::wstring();
    std::wstring text = L"default ";
    text += *def->shipped ? showValue(*def, def->shipped) : std::wstring(L"(empty)");
    if (*def->lo && *def->hi) {
        text += L"  \x00b7  " + showValue(*def, def->lo) + L" to " + showValue(*def, def->hi);
        // On a per-headset list the bounds are one entry's, and a bare number
        // in the box names no headset and is ignored. Say which it bounds
        // rather than inviting somebody to type the bound itself.
        if (def->headset) text += L" per headset";
    }
    return text;
}

const std::vector<SettingDef>& settingDefs() {
    static const std::vector<SettingDef> defs(
        kSettings, kSettings + sizeof(kSettings) / sizeof(kSettings[0]));
    return defs;
}

bool SettingsModel::load(const std::wstring& gameDir, const std::string& profile) {
    m_gameDir = gameDir;
    m_backedUp = false;
    m_profile = profile;
    m_iniPath = settingsPathFor(gameDir, profile);
    m_sharedPath = profile == "flat" ? settingsPathFor(gameDir, "vr") : std::wstring();
    bool fromShared = false;
    m_text = currentText(&fromShared);
    m_loaded = true;
    m_error.clear();
    refreshRows();
    return true;
}

std::string SettingsModel::currentText(bool* fromShared) const {
    *fromShared = false;
    if (fileExists(m_iniPath)) return readTextFile(m_iniPath);
    if (!m_sharedPath.empty() && fileExists(m_sharedPath)) {
        *fromShared = true;
        return readTextFile(m_sharedPath);
    }
    return std::string();
}

void SettingsModel::refreshRows() {
    m_rows.clear();
    // The flat selection shows the rows the flat profile reads (flat_profile_keys.h, the runtime's own list); the
    // VR selection shows every setting.
    const bool flatRows = m_profile == "flat";
    for (const SettingDef& def : settingDefs()) {
        if (flatRows) {
            const std::string dotted = std::string(def.section) + "." + def.key;
            // fix.temporal_aa is read by the flat runtime through Config::requestedTemporalMode, which skips the allow-list.
            if (!edvr::flatProfileReadsKey(dotted.c_str()) && dotted != "fix.temporal_aa") continue;
        } else {
            // XeSS Frame Generation cannot work under OpenXR: the flat selection's rows only (the settings the VR runtime never reads).
            const std::string vrDotted = std::string(def.section) + "." + def.key;
            if (vrDotted == "fix.xess_frame_generation" || vrDotted == "fix.xess_generated_frames") continue;
        }
        SettingRow row;
        row.def = &def;
        row.choices = splitChoices(def.choices);

        // What the file says, or -- for a setting the user's ini does not carry
        // at all -- what this build ships. An un-migrated file is read the way
        // the runtime reads it: through the moved-from map, ignoring an old
        // line that still carries its retired default (that is an un-updated
        // file, not a choice -- the field saw the window claim splash while
        // the runtime honoured a stale menu_backdrop = stock).
        const std::string dotted = std::string(def.section) + "." + def.key;
        const char* kAbsent = "\x01";
        row.value = iniValue(m_text, dotted, kAbsent);
        if (row.value == kAbsent) {
            row.value = def.shipped;
            for (const auto& mv : kMovedSettings) {
                if (!mv[0][0] || dotted != mv[1]) continue;
                const std::string oldValue = iniValue(m_text, mv[0], kAbsent);
                if (oldValue == kAbsent) continue;
                if (mv[2][0] && oldValue == mv[2]) continue;  // stale default
                row.value = oldValue;
                break;
            }
        }
        row.isRecommended = sameValue(def, row.value, def.recommended);
        m_rows.push_back(row);
    }
}

bool SettingsModel::set(size_t index, const std::string& value) {
    if (index >= m_rows.size()) return false;
    const SettingDef& def = *m_rows[index].def;
    const std::string dotted = std::string(def.section) + "." + def.key;

    // Re-read the file first: the install/update path rewrites the settings file
    // -- migrating renamed keys as it goes -- and a model cached when this
    // window opened would write that pre-update text straight back over the
    // migration, one stale byte at a time. Measured in the field, 2026-08-28:
    // an update at 21:45:22, a settings save at 21:45:39, and the migrated
    // file was gone.
    const std::string leaf = toUtf8(leafOf(m_iniPath));
    bool fromShared = false;
    m_text = currentText(&fromShared);

    // The merge engine, used for one value: merging a document with itself is a
    // no-op (installer_test proves it), so the only change is the forced value
    // -- written into the line where the setting already lives, uncommented if
    // it was an expert default, with the file's layout and every comment
    // untouched. Exactly what a careful hand edit would do.
    std::string source = m_text;
    if (source.empty()) {
        m_error = leaf + " is not there yet -- install ED-UP into this folder first.";
        return false;
    }
    // One copy of the file as it was when this window opened, before the first
    // change of the session. The install path backs the settings file up;
    // changing a setting did not, and a settings screen is exactly where
    // somebody changes several things and then wants the one they had. A flat
    // file that does not exist yet has nothing to back up -- this change starts
    // it from the shared edvr.ini, which is not written at all.
    if (!m_backedUp) {
        if (!fromShared) {
            const std::wstring backupDir =
                joinPath(backupRootPath(m_gameDir), L"settings-" + timestampName());
            if (ensureBackupDir(backupDir)) {
                CopyFileW(m_iniPath.c_str(), joinPath(backupDir, leafOf(m_iniPath)).c_str(), FALSE);
            }
        }
        m_backedUp = true;   // tried once; a failure here must not block editing
    }

    MergeReport report;
    const std::string updated = mergeIni(source, source, &source, {{dotted, value}}, &report);

    // Clear read-only before atomic replace (some store verifiers leave files read-only).
    const DWORD attrs = GetFileAttributesW(m_iniPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(m_iniPath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    std::wstring why;
    if (!writeFileAtomic(m_iniPath, updated, &why)) {
        m_error = "Could not write " + leaf + ": " + toUtf8(why) +
                  ". If the game folder is under Program Files, run the installer as "
                  "administrator.";
        return false;
    }
    m_text = updated;
    m_error.clear();
    refreshRows();
    return true;
}

}  // namespace edvr::installer
