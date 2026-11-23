#include "edhm_d3dx.h"

#include <cctype>
#include <cstring>
#include <vector>

namespace edvr::installer {
namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool asciiEq(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && isSpace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

struct RawLine {
    std::string body;  // text without line ending
    std::string eol;   // "\r\n", "\n", or "" for last line without newline
};

std::vector<RawLine> splitLines(const std::string& text, bool* hasBom, std::string* defaultEol) {
    size_t pos = 0;
    *hasBom = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
              static_cast<unsigned char>(text[1]) == 0xBB &&
              static_cast<unsigned char>(text[2]) == 0xBF;
    if (*hasBom) pos = 3;

    size_t crlfCount = 0;
    size_t lfCount = 0;
    std::vector<RawLine> lines;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        RawLine line;
        if (nl == std::string::npos) {
            line.body = text.substr(pos);
            pos = text.size();
        } else {
            if (nl > pos && text[nl - 1] == '\r') {
                line.body = text.substr(pos, nl - 1 - pos);
                line.eol = "\r\n";
                ++crlfCount;
            } else {
                line.body = text.substr(pos, nl - pos);
                line.eol = "\n";
                ++lfCount;
            }
            pos = nl + 1;
        }
        lines.push_back(std::move(line));
    }
    *defaultEol = (lfCount > crlfCount && crlfCount == 0) ? "\n" : "\r\n";
    return lines;
}

std::string joinLines(const std::vector<RawLine>& lines, bool hasBom) {
    std::string out;
    if (hasBom) out.append("\xEF\xBB\xBF", 3);
    for (const RawLine& l : lines) {
        out += l.body;
        out += l.eol;
    }
    return out;
}

bool parseSectionHeader(const std::string& body, std::string* sectionName) {
    size_t i = 0;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
    if (i >= body.size() || body[i] != '[') return false;
    const size_t close = body.find(']', i + 1);
    if (close == std::string::npos) return false;
    size_t tail = close + 1;
    while (tail < body.size() && (body[tail] == ' ' || body[tail] == '\t')) ++tail;
    if (tail < body.size() && body[tail] != ';') return false;
    *sectionName = trim(body.substr(i + 1, close - (i + 1)));
    return true;
}

struct ParsedKeyLine {
    bool isKey = false;
    bool commented = false;
    std::string key;
    std::string value;
    std::string prefix;        // everything up to and including '=' and spaces after '='
    std::string inlineComment; // whitespace + ';...' at end of line, if present
};

ParsedKeyLine parseKeyLine(const std::string& body) {
    ParsedKeyLine out;
    size_t i = 0;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
    if (i >= body.size()) return out;

    bool commented = false;
    if (body[i] == ';') {
        commented = true;
        ++i;
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == ';')) ++i;
    }
    if (i >= body.size() || body[i] == '[') return out;

    const size_t eq = body.find('=', i);
    if (eq == std::string::npos) return out;
    const std::string rawKey = trim(body.substr(i, eq - i));
    if (rawKey.empty()) return out;
    for (char c : rawKey) {
        if (c == ';' || c == '[' || c == ']' || isSpace(c)) return out;
    }

    size_t valStart = eq + 1;
    while (valStart < body.size() && (body[valStart] == ' ' || body[valStart] == '\t')) ++valStart;

    size_t commentPos = body.find(';', valStart);
    size_t valEnd = commentPos == std::string::npos ? body.size() : commentPos;
    while (valEnd > valStart && (body[valEnd - 1] == ' ' || body[valEnd - 1] == '\t')) --valEnd;

    out.isKey = true;
    out.commented = commented;
    out.key = rawKey;
    out.value = body.substr(valStart, valEnd - valStart);
    out.prefix = body.substr(0, valStart);
    if (commentPos != std::string::npos) {
        out.inlineComment = body.substr(valEnd);
    }
    return out;
}

}  // namespace

bool isUtf16D3dx(const std::string& bytes) {
    if (bytes.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
        const unsigned char b1 = static_cast<unsigned char>(bytes[1]);
        if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) return true;
    }
    return bytes.find('\0') != std::string::npos;
}

D3dxKeyRead readD3dxKey(const std::string& iniText, const char* section, const char* key) {
    D3dxKeyRead result;
    if (isUtf16D3dx(iniText)) return result;
    bool hasBom = false;
    std::string defaultEol;
    const std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    bool inFirstSection = false;
    bool foundSection = false;
    for (const RawLine& line : lines) {
        std::string sec;
        if (parseSectionHeader(line.body, &sec)) {
            if (inFirstSection) break;
            if (!foundSection && asciiEq(sec, section)) {
                inFirstSection = true;
                foundSection = true;
            }
            continue;
        }
        if (!inFirstSection) continue;
        const ParsedKeyLine parsed = parseKeyLine(line.body);
        if (parsed.isKey && !parsed.commented && asciiEq(parsed.key, key)) {
            result.present = true;
            result.value = parsed.value;
            return result;
        }
    }
    return result;
}

std::string setD3dxKey(const std::string& iniText, const char* section, const char* key,
                       const std::string& value) {
    if (isUtf16D3dx(iniText)) return iniText;
    bool hasBom = false;
    std::string defaultEol;
    std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    size_t firstActiveMatch = static_cast<size_t>(-1);
    ParsedKeyLine firstActiveParsed;
    size_t firstCommentedMatch = static_cast<size_t>(-1);
    size_t firstSectionEnd = static_cast<size_t>(-1);
    bool inFirstSection = false;
    bool foundSection = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sec;
        if (parseSectionHeader(lines[i].body, &sec)) {
            if (inFirstSection) {
                firstSectionEnd = i;
                break;
            }
            if (!foundSection && asciiEq(sec, section)) {
                inFirstSection = true;
                foundSection = true;
            }
            continue;
        }
        if (!inFirstSection) continue;
        const ParsedKeyLine parsed = parseKeyLine(lines[i].body);
        if (parsed.isKey && asciiEq(parsed.key, key)) {
            if (!parsed.commented) {
                if (firstActiveMatch == static_cast<size_t>(-1)) {
                    firstActiveMatch = i;
                    firstActiveParsed = parsed;
                }
            } else if (firstCommentedMatch == static_cast<size_t>(-1)) {
                firstCommentedMatch = i;
            }
        }
    }
    if (inFirstSection && firstSectionEnd == static_cast<size_t>(-1)) firstSectionEnd = lines.size();

    if (firstActiveMatch != static_cast<size_t>(-1)) {
        lines[firstActiveMatch].body = firstActiveParsed.prefix + value + firstActiveParsed.inlineComment;
        return joinLines(lines, hasBom);
    }

    const std::string newBody = std::string(key) + " = " + value;
    if (firstCommentedMatch != static_cast<size_t>(-1)) {
        if (lines[firstCommentedMatch].eol.empty()) lines[firstCommentedMatch].eol = defaultEol;
        RawLine ins;
        ins.body = newBody;
        ins.eol = defaultEol;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(firstCommentedMatch + 1), std::move(ins));
        return joinLines(lines, hasBom);
    }

    if (firstSectionEnd != static_cast<size_t>(-1)) {
        size_t insertAt = firstSectionEnd;
        while (insertAt > 0 && trim(lines[insertAt - 1].body).empty()) --insertAt;
        if (insertAt > 0 && lines[insertAt - 1].eol.empty()) lines[insertAt - 1].eol = defaultEol;
        RawLine ins;
        ins.body = newBody;
        ins.eol = defaultEol;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(ins));
        return joinLines(lines, hasBom);
    }

    // Section does not exist yet: append cleanly at EOF.
    if (!lines.empty()) {
        if (lines.back().eol.empty()) lines.back().eol = defaultEol;
        if (!trim(lines.back().body).empty()) {
            RawLine blank;
            blank.eol = defaultEol;
            lines.push_back(std::move(blank));
        }
    }
    RawLine hdr;
    hdr.body = std::string("[") + section + "]";
    hdr.eol = defaultEol;
    lines.push_back(std::move(hdr));

    RawLine entry;
    entry.body = newBody;
    entry.eol = defaultEol;
    lines.push_back(std::move(entry));
    return joinLines(lines, hasBom);
}

std::string removeD3dxKey(const std::string& iniText, const char* section, const char* key) {
    if (isUtf16D3dx(iniText)) return iniText;
    bool hasBom = false;
    std::string defaultEol;
    std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    bool inFirstSection = false;
    bool foundSection = false;
    size_t firstActiveMatch = static_cast<size_t>(-1);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sec;
        if (parseSectionHeader(lines[i].body, &sec)) {
            if (inFirstSection) break;
            if (!foundSection && asciiEq(sec, section)) {
                inFirstSection = true;
                foundSection = true;
            }
            continue;
        }
        if (!inFirstSection) continue;
        const ParsedKeyLine parsed = parseKeyLine(lines[i].body);
        if (parsed.isKey && !parsed.commented && asciiEq(parsed.key, key)) {
            firstActiveMatch = i;
            break;
        }
    }
    if (firstActiveMatch == static_cast<size_t>(-1)) return iniText;
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(firstActiveMatch));
    return joinLines(lines, hasBom);
}

EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText) {
    EdhmD3dxBackup b;
    b.saved = true;
    const D3dxKeyRead llr = readD3dxKey(iniText, "System", "load_library_redirect");
    b.loadLibraryRedirect.present = llr.present;
    b.loadLibraryRedirect.value = llr.value;

    const D3dxKeyRead apu = readD3dxKey(iniText, "System", "allow_platform_update");
    b.allowPlatformUpdate.present = apu.present;
    b.allowPlatformUpdate.value = apu.value;

    const D3dxKeyRead ups = readD3dxKey(iniText, "Device", "upscaling");
    b.upscaling.present = ups.present;
    b.upscaling.value = ups.value;
    return b;
}

std::string applyEdhmD3dxCompat(const std::string& iniText) {
    std::string out = setD3dxKey(iniText, "System", "load_library_redirect", "0");
    out = setD3dxKey(out, "System", "allow_platform_update", "1");
    out = setD3dxKey(out, "Device", "upscaling", "0");
    return out;
}

std::string restoreEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxBackup& backup) {
    if (!backup.anySaved() || isUtf16D3dx(iniText)) return iniText;
    auto restoreIfStillOurs = [](const std::string& text, const char* section, const char* key,
                                 const EdhmD3dxKeyBackup& saved, const char* expectedEdupValue) {
        const D3dxKeyRead cur = readD3dxKey(text, section, key);
        if (!cur.present || cur.value != expectedEdupValue) return text;
        return saved.present ? setD3dxKey(text, section, key, saved.value)
                             : removeD3dxKey(text, section, key);
    };
    std::string out = iniText;
    if (backup.saved) {
        out = restoreIfStillOurs(out, "System", "load_library_redirect", backup.loadLibraryRedirect, "0");
        out = restoreIfStillOurs(out, "System", "allow_platform_update", backup.allowPlatformUpdate, "1");
        out = restoreIfStillOurs(out, "Device", "upscaling", backup.upscaling, "0");
    }
    if (backup.loggingReloadSaved) {
        out = restoreIfStillOurs(out, "Logging", "show_reload_message", backup.showReloadMessage, "0");
    }
    if (backup.loggingWarningsSaved) {
        out = restoreIfStillOurs(out, "Logging", "show_warnings", backup.showWarnings, "0");
    }
    return out;
}

std::string restoreEdhmLoggingKey(const std::string& iniText, const char* key,
                                  const EdhmD3dxKeyBackup& saved, bool hasSaved) {
    if (isUtf16D3dx(iniText)) return iniText;
    if (hasSaved) {
        const D3dxKeyRead cur = readD3dxKey(iniText, "Logging", key);
        if (!cur.present || cur.value != "0") return iniText;
        return saved.present ? setD3dxKey(iniText, "Logging", key, saved.value)
                             : removeD3dxKey(iniText, "Logging", key);
    }
    return setD3dxKey(iniText, "Logging", key, "1");
}

bool readEdhmHideReloadMessages(const std::string& iniText) {
    const D3dxKeyRead r = readD3dxKey(iniText, "Logging", "show_reload_message");
    return r.present && r.value == "0";
}

std::string setEdhmHideReloadMessages(const std::string& iniText, bool hide) {
    return setD3dxKey(iniText, "Logging", "show_reload_message", hide ? "0" : "1");
}

bool readEdhmHideWarnings(const std::string& iniText) {
    const D3dxKeyRead r = readD3dxKey(iniText, "Logging", "show_warnings");
    return r.present && r.value == "0";
}

std::string setEdhmHideWarnings(const std::string& iniText, bool hide) {
    return setD3dxKey(iniText, "Logging", "show_warnings", hide ? "0" : "1");
}

}  // namespace edvr::installer
