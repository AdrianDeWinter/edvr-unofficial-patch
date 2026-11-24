// The text of a hotkey value (hotkey.h): the keyboard's key names, the pad and
// joystick spellings, and the parse and format of a whole value. Split out of
// hotkey.cpp (2026-11-21) so a program without EDVR's log -- the launcher's
// Settings tab -- checks a value with the grammar the game reads rather than a
// copy of it that drifts: the launcher's own copy accepted PGUP, which this
// parser does not, and left the menu key unbound. hotkey.cpp points the note
// sink at the log; with no sink the "not a key name" lines are not said.
#include "hotkey.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <windows.h>

#include "pad_names.h"

namespace edvr {

namespace {

HotkeyNoteFn g_noteSink = nullptr;

// One line for the sink, formatted here so the sink needs no varargs. 1200 is
// the most Log::note writes of a line anyway.
void hotkeyNote(const char* fmt, ...) {
    const HotkeyNoteFn sink = g_noteSink;
    if (!sink) return;
    char line[1200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    sink(line);
}

}  // namespace

void hotkeySetNoteSink(HotkeyNoteFn fn) { g_noteSink = fn; }

// ---------------------------------------------------------------------------
// Key names. Hoisted out of virtualKeyFromName (2026-10-08) so the settings
// menu can write a captured key back as a name this parser reads to the same
// key: one table, both directions.

namespace {

struct KeyNameEntry { const char* name; int vk; };
// The FIRST name listed for a key is the one a capture writes.
const KeyNameEntry kNamedKeys[] = {
    {"F1", VK_F1},   {"F2", VK_F2},   {"F3", VK_F3},   {"F4", VK_F4},
    {"F5", VK_F5},   {"F6", VK_F6},   {"F7", VK_F7},   {"F8", VK_F8},
    {"F9", VK_F9},   {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
    {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
    {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},
    {"SCROLLLOCK", VK_SCROLL}, {"SCROLL", VK_SCROLL},
    {"PAUSE", VK_PAUSE},       {"NUMLOCK", VK_NUMLOCK},
    {"INSERT", VK_INSERT},     {"HOME", VK_HOME},
    {"END", VK_END},           {"DELETE", VK_DELETE},
    {"PAGEUP", VK_PRIOR},      {"PAGEDOWN", VK_NEXT},
    {"NUMPAD0", VK_NUMPAD0},   {"NUMPAD1", VK_NUMPAD1},
    {"NUMPAD2", VK_NUMPAD2},   {"NUMPAD3", VK_NUMPAD3},
    {"NUMPAD4", VK_NUMPAD4},   {"NUMPAD5", VK_NUMPAD5},
    {"NUMPAD6", VK_NUMPAD6},   {"NUMPAD7", VK_NUMPAD7},
    {"NUMPAD8", VK_NUMPAD8},   {"NUMPAD9", VK_NUMPAD9},
    {"MULTIPLY", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE},
    {"ADD", VK_ADD},           {"SUBTRACT", VK_SUBTRACT},
    // The arrow keys were missing, and Elite binds the camera-view cycle to
    // one of them by default. Asking for RIGHT fell through to the
    // unrecognised path below, which returned "no key" in silence -- so the
    // feature simply never fired and nothing said why.
    {"RIGHT", VK_RIGHT},       {"LEFT", VK_LEFT},
    {"UP", VK_UP},             {"DOWN", VK_DOWN},
    {"RIGHTARROW", VK_RIGHT},  {"LEFTARROW", VK_LEFT},
    {"UPARROW", VK_UP},        {"DOWNARROW", VK_DOWN},
    {"SPACE", VK_SPACE},       {"TAB", VK_TAB},
    {"ENTER", VK_RETURN},      {"RETURN", VK_RETURN},
    {"BACKSPACE", VK_BACK},    {"ESCAPE", VK_ESCAPE},
    {"ESC", VK_ESCAPE},        {"CAPSLOCK", VK_CAPITAL},
    {"PRINTSCREEN", VK_SNAPSHOT}, {"APPS", VK_APPS},
    {"MENU_KEY", VK_APPS},     {"DECIMAL", VK_DECIMAL},
    {"NUMPADDOT", VK_DECIMAL},
};

// The punctuation row, by NAME. Resolved through the character rather than
// through a hard-coded VK_OEM_* code, because the OEM codes are positions
// on a US keyboard and these names describe CHARACTERS -- on another
// layout the character lives on a different physical key, and the one the
// player actually presses is the one that types it.
struct CharName { const char* name; wchar_t ch; };
const CharName kCharNames[] = {
    {"BACKSLASH", L'\\'},   {"SLASH", L'/'},
    {"LEFTBRACKET", L'['},  {"RIGHTBRACKET", L']'},
    {"SEMICOLON", L';'},    {"APOSTROPHE", L'\''},
    {"QUOTE", L'\''},       {"COMMA", L','},
    {"PERIOD", L'.'},       {"DOT", L'.'},
    {"GRAVE", L'`'},        {"BACKTICK", L'`'},
    {"TILDE", L'`'},        {"MINUS", L'-'},
    {"DASH", L'-'},         {"EQUALS", L'='},
    {"PLUS", L'='},
    // '#' by name, because bare ';' and '#' after "= " are eaten by the
    // ini's own trailing-comment rule -- SEMICOLON and HASH are the
    // reliable spellings in a config value, and the ini says so. (On a
    // UK layout '#' is its own physical key, so it is a real binding.)
    {"HASH", L'#'},         {"POUND", L'#'},
};

// Set while the formatter checks its own output, and by a quiet parse: the
// "not a key name" line is for a person's typo, not for a probe.
thread_local int t_quietKeyNames = 0;
struct QuietScope {
    QuietScope() { ++t_quietKeyNames; }
    ~QuietScope() { --t_quietKeyNames; }
};

bool isVkOem(int vk) {
    return (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2;
}

}  // namespace

size_t hotkeyKeyName(int vk, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    char name[24];
    name[0] = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(name, sizeof(name), "%c", vk);
    } else {
        for (const KeyNameEntry& e : kNamedKeys) {
            if (e.vk == vk) {
                snprintf(name, sizeof(name), "%s", e.name);
                break;
            }
        }
    }
    if (!name[0] && isVkOem(vk)) {
        // The character this key types on the active layout, and the first
        // name for it. The round trip below is the check that the layout
        // agrees; a key it cannot name falls through to the raw code.
        const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;
        for (const CharName& e : kCharNames) {
            if (static_cast<UINT>(e.ch) == ch) {
                snprintf(name, sizeof(name), "%s", e.name);
                break;
            }
        }
    }
    if (name[0]) {
        QuietScope quiet;
        uint32_t m = 0;
        if (virtualKeyFromName(name, &m) != vk || m != 0) name[0] = 0;
    }
    if (!name[0]) snprintf(name, sizeof(name), "0x%02X", vk & 0xFF);
    snprintf(out, cap, "%s", name);
    return strlen(out);
}

int virtualKeyFromName(const char* name) { return virtualKeyFromName(name, nullptr); }

int virtualKeyFromName(const char* name, uint32_t* mods) {
    if (mods) *mods = 0;
    if (!name || !*name) return 0;
    // A pad or joystick spelling is a hotkey value too, just not a keyboard
    // one: say nothing, the caller that asked for a keyboard key gets "none".
    if (hotkeyLooksNonKeyboard(name)) return 0;

    // Modifiers are PEELED FROM THE FRONT, not split out of the whole string.
    //
    // The old parser split on every '+' and '-' and called the last piece the
    // key -- which made '-', '+' and '=' unbindable as keys, because the
    // characters themselves were separators. A field user bound '\\' and '['
    // (6az), and the punctuation row is exactly where people put camera keys.
    // Peeling instead means a separator only counts when it FOLLOWS a
    // modifier word, so CTRL+ALT+SPACE and CONTROL-MENU-SPACE parse exactly
    // as before while SHIFT+- means shift and the minus key, and a lone '-'
    // is just the minus key.
    //
    // A leading token that is not a modifier still rejects the WHOLE binding
    // (CTRL+WOMBAT+SPACE binds nothing and says so) -- taking the last part
    // and ignoring the rest would turn a typo into a different, live binding.
    {
        std::string s(name);
        uint32_t m = 0;
        size_t pos = 0;
        while (true) {
            // Skip leading spaces, read a word, upper-case it.
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
            size_t w = pos;
            while (w < s.size() && (isalpha(static_cast<unsigned char>(s[w])))) ++w;
            std::string word = s.substr(pos, w - pos);
            for (char& c : word) c = static_cast<char>(toupper(c));
            uint32_t bit = 0;
            if (word == "CTRL" || word == "CONTROL") bit = kHotkeyCtrl;
            else if (word == "ALT" || word == "MENU") bit = kHotkeyAlt;
            else if (word == "SHIFT") bit = kHotkeyShift;
            if (!bit) break;
            // A modifier word must be FOLLOWED by a separator to be one --
            // otherwise it is (an attempt at) the key itself.
            size_t after = w;
            while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
            if (after >= s.size() || (s[after] != '+' && s[after] != '-')) break;
            m |= bit;
            pos = after + 1;
        }
        if (pos > 0) {
            // Something was peeled: the remainder, trimmed, is the key.
            size_t b = s.find_first_not_of(" \t", pos);
            size_t e = s.find_last_not_of(" \t");
            std::string last = (b == std::string::npos)
                                   ? std::string()
                                   : s.substr(b, e - b + 1);
            if (last.empty()) {
                if (!t_quietKeyNames)
                    hotkeyNote("hotkey: \"%s\" has modifiers but no key. "
                                    "Modifiers are CTRL, ALT and SHIFT, joined with "
                                    "'+', and the key comes last -- CTRL+ALT+SPACE.",
                                    name);
                return 0;
            }
            const int vk = virtualKeyFromName(last.c_str(), nullptr);
            if (vk && mods) *mods = m;
            return vk;
        }
    }

    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        return static_cast<int>(strtol(name, nullptr, 16));
    }

    for (const KeyNameEntry& e : kNamedKeys) {
        if (_stricmp(name, e.name) == 0) return e.vk;
    }

    wchar_t toScan = 0;
    for (const CharName& e : kCharNames) {
        if (_stricmp(name, e.name) == 0) { toScan = e.ch; break; }
    }

    // Single printable character: letters and digits map directly; anything
    // else -- '\\', '[', ';', '-', whatever the player's camera key types --
    // goes through the keyboard layout.
    if (!toScan && name[1] == '\0') {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        if (c > 0x20 && c < 0x7F) toScan = static_cast<wchar_t>(c);
    }
    if (toScan) {
        // VkKeyScanW asks the ACTIVE layout which key types this character.
        // The low byte is the physical key; the high byte says which
        // modifiers the character itself needs, and that half is deliberately
        // ignored -- a binding names a KEY, so '|' and '\\' are the same
        // binding, and whether to require Shift is what the CTRL/ALT/SHIFT
        // prefix is for. -1 means no key on this layout types it, which
        // falls through to the say-so below rather than binding nothing
        // silently.
        const SHORT scan = VkKeyScanW(toScan);
        if (scan != -1) return scan & 0xFF;
    }
    // A name nobody recognises is NOT the same as no key, and returning 0 for
    // both is what made this invisible: a typo, or a key this table has never
    // heard of, produced a feature that silently never fired. An empty setting
    // is a choice and returns 0 above, without comment; getting here means
    // somebody asked for something specific and did not get it.
    if (!t_quietKeyNames)
        hotkeyNote("hotkey \"%s\" is not a key name ED-UP knows, so nothing is "
                        "bound. Try F1-F24, SCROLLLOCK, PAUSE, NUMLOCK, CAPSLOCK, "
                        "PRINTSCREEN, INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, "
                        "LEFT, RIGHT, UP, DOWN, SPACE, TAB, ENTER, ESCAPE, "
                        "NUMPAD0-9, DECIMAL, any single character your keyboard "
                        "types (letters, digits, punctuation like \\ [ ] ; ' , . "
                        "/ ` - =), names for those (BACKSLASH, LEFTBRACKET, "
                        "SEMICOLON, ...), or 0x## for a raw virtual-key code. A "
                        "gamepad button is GamePad_Back and the like; a HOTAS or "
                        "joystick button is the device and the button, "
                        "231D0200:Joy_12 (the settings menu's Hotkeys page "
                        "writes these for you).",
                        name);
    return 0;
}

// ---------------------------------------------------------------------------
// Pad and joystick spellings, and the parse/format of a whole hotkey value.

namespace {

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

const char* skipSpaces(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

const char* const kPovDirName[4] = {"Up", "Right", "Down", "Left"};

}  // namespace

bool hotkeyJoyInputFromEliteKey(const char* key, uint16_t* input) {
    if (!key || _strnicmp(key, "Joy_", 4) != 0) return false;
    const char* r = key + 4;
    if (_strnicmp(r, "POV", 3) == 0) {
        r += 3;
        if (*r < '1' || *r > '0' + kJoyHatCount) return false;
        const int hat = *r - '1';
        ++r;
        for (int dir = 0; dir < 4; ++dir) {
            if (_stricmp(r, kPovDirName[dir]) == 0) {
                if (input) *input = static_cast<uint16_t>(kJoyPovBase + hat * 4 + dir);
                return true;
            }
        }
        return false;
    }
    // Joy_12: a plain decimal 1..128, no sign, no leading zero, nothing after.
    if (*r < '1' || *r > '9') return false;
    int n = 0;
    for (; *r; ++r) {
        if (*r < '0' || *r > '9') return false;
        n = n * 10 + (*r - '0');
        if (n > kJoyButtonCount) return false;
    }
    if (input) *input = static_cast<uint16_t>(n - 1);
    return true;
}

size_t hotkeyJoyInputName(uint16_t input, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (input < kJoyButtonCount) {
        snprintf(out, cap, "Joy_%d", input + 1);
    } else if (input < kJoyInputCount) {
        const int k = input - kJoyPovBase;
        snprintf(out, cap, "Joy_POV%d%s", k / 4 + 1, kPovDirName[k % 4]);
    }
    return strlen(out);
}

bool hotkeyJoyDeviceFromText(const char* text, uint32_t* device) {
    if (!text) return false;
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        if (!isHexDigit(text[i])) return false;
        const char c = text[i];
        v = (v << 4) | static_cast<uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    if (text[8] != 0 && text[8] != ':') return false;
    if (device) *device = v;
    return true;
}

void hotkeyJoyDeviceText(uint32_t device, char out[9]) {
    snprintf(out, 9, "%08X", device);
}

bool hotkeyLooksNonKeyboard(const char* text) {
    if (!text) return false;
    text = skipSpaces(text);
    if (_strnicmp(text, "GamePad_", 8) == 0) return true;
    for (int i = 0; i < 8; ++i) {
        if (!isHexDigit(text[i])) return false;
    }
    return text[8] == ':';
}

bool hotkeyParseBinding(const char* text, HotkeyBinding* out, bool quiet) {
    HotkeyBinding b;
    bool ok = true;
    if (text) text = skipSpaces(text);
    if (!text || !*text) {
        // Empty: an unbound hotkey, which is a choice and says nothing.
    } else if (_strnicmp(text, "GamePad_", 8) == 0) {
        std::string name(text);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        const PadName* p = padNameFind(name.c_str());
        if (p) {
            b.kind = HotkeyKind::Pad;
            b.padButtons = p->buttons;
            b.padTrigger = p->trigger;
        } else {
            ok = false;
            if (!quiet)
                hotkeyNote("hotkey \"%s\" is not a gamepad button ED-UP knows, so nothing is "
                                "bound. Try GamePad_FaceDown, _FaceRight, _FaceLeft, _FaceUp, "
                                "_DPadUp, _DPadDown, _DPadLeft, _DPadRight, _Back, _Start, "
                                "_LBumper, _RBumper, _LThumb, _RThumb, _LTrigger or _RTrigger.",
                                name.c_str());
        }
    } else if (hotkeyLooksNonKeyboard(text)) {
        uint32_t device = 0;
        uint16_t input = 0;
        std::string rest(text + 9);
        const size_t first = rest.find_first_not_of(" \t");
        rest = first == std::string::npos ? std::string() : rest.substr(first);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
        if (hotkeyJoyDeviceFromText(text, &device) && hotkeyJoyInputFromEliteKey(rest.c_str(), &input)) {
            b.kind = HotkeyKind::Joy;
            b.joyDevice = device;
            b.joyInput = input;
        } else {
            ok = false;
            if (!quiet)
                hotkeyNote("hotkey \"%s\" is not a joystick button ED-UP knows, so nothing is "
                                "bound. It is the device Elite writes in your bindings (eight hex "
                                "digits: vendor then product) and a button, 231D0200:Joy_12, or a "
                                "hat, 231D0200:Joy_POV1Up (Right, Down, Left).",
                                text);
        }
    } else {
        uint32_t mods = 0;
        int vk = 0;
        if (quiet) {
            QuietScope scope;
            vk = virtualKeyFromName(text, &mods);
        } else {
            vk = virtualKeyFromName(text, &mods);
        }
        if (vk) {
            b.kind = HotkeyKind::Key;
            b.vk = vk;
            b.mods = mods;
        } else {
            ok = false;
        }
    }
    if (out) *out = b;
    return ok;
}

size_t hotkeyFormatBinding(const HotkeyBinding& b, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    switch (b.kind) {
        case HotkeyKind::Key: {
            std::string s;
            if (b.mods & kHotkeyCtrl) s += "CTRL+";
            if (b.mods & kHotkeyAlt) s += "ALT+";
            if (b.mods & kHotkeyShift) s += "SHIFT+";
            char name[24];
            hotkeyKeyName(b.vk, name, sizeof(name));
            s += name;
            snprintf(out, cap, "%s", s.c_str());
            break;
        }
        case HotkeyKind::Pad: {
            const char* name = padNameOf(b.padButtons, b.padTrigger);
            if (name) snprintf(out, cap, "%s", name);
            break;
        }
        case HotkeyKind::Joy: {
            char dev[9];
            char key[24];
            hotkeyJoyDeviceText(b.joyDevice, dev);
            if (hotkeyJoyInputName(b.joyInput, key, sizeof(key))) snprintf(out, cap, "%s:%s", dev, key);
            break;
        }
        default:
            break;
    }
    return strlen(out);
}

}  // namespace edvr
