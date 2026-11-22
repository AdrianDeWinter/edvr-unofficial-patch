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

D3dxKeyRead readD3dxKey(const std::string& iniText, const char* section, const char* key) {
    D3dxKeyRead result;
    bool hasBom = false;
    std::string defaultEol;
    const std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    std::string curSection;
    for (const RawLine& line : lines) {
        std::string sec;
        if (parseSectionHeader(line.body, &sec)) {
            curSection = sec;
            continue;
        }
        if (!asciiEq(curSection, section)) continue;
        const ParsedKeyLine parsed = parseKeyLine(line.body);
        if (parsed.isKey && !parsed.commented && asciiEq(parsed.key, key)) {
            result.present = true;
            result.value = parsed.value;
        }
    }
    return result;
}

std::string setD3dxKey(const std::string& iniText, const char* section, const char* key,
                       const std::string& value) {
    bool hasBom = false;
    std::string defaultEol;
    std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    std::string curSection;
    bool updatedActive = false;
    size_t lastCommentedMatch = static_cast<size_t>(-1);
    size_t lastSectionEnd = static_cast<size_t>(-1);
    bool inTargetSection = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sec;
        if (parseSectionHeader(lines[i].body, &sec)) {
            if (inTargetSection) lastSectionEnd = i;
            curSection = sec;
            inTargetSection = asciiEq(curSection, section);
            continue;
        }
        if (!inTargetSection) continue;
        const ParsedKeyLine parsed = parseKeyLine(lines[i].body);
        if (parsed.isKey && asciiEq(parsed.key, key)) {
            if (!parsed.commented) {
                lines[i].body = parsed.prefix + value + parsed.inlineComment;
                updatedActive = true;
            } else {
                lastCommentedMatch = i;
            }
        }
    }
    if (inTargetSection) lastSectionEnd = lines.size();

    if (updatedActive) return joinLines(lines, hasBom);

    const std::string newBody = std::string(key) + " = " + value;
    if (lastCommentedMatch != static_cast<size_t>(-1)) {
        if (lines[lastCommentedMatch].eol.empty()) lines[lastCommentedMatch].eol = defaultEol;
        RawLine ins;
        ins.body = newBody;
        ins.eol = defaultEol;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(lastCommentedMatch + 1), std::move(ins));
        return joinLines(lines, hasBom);
    }

    if (lastSectionEnd != static_cast<size_t>(-1)) {
        size_t insertAt = lastSectionEnd;
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
    bool hasBom = false;
    std::string defaultEol;
    std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    std::vector<RawLine> kept;
    kept.reserve(lines.size());
    std::string curSection;
    for (RawLine& line : lines) {
        std::string sec;
        if (parseSectionHeader(line.body, &sec)) {
            curSection = sec;
            kept.push_back(std::move(line));
            continue;
        }
        if (asciiEq(curSection, section)) {
            const ParsedKeyLine parsed = parseKeyLine(line.body);
            if (parsed.isKey && !parsed.commented && asciiEq(parsed.key, key)) {
                continue;
            }
        }
        kept.push_back(std::move(line));
    }
    return joinLines(kept, hasBom);
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
    if (!backup.saved) return iniText;
    auto restoreOne = [](const std::string& text, const char* section, const char* key,
                         const EdhmD3dxKeyBackup& saved) {
        return saved.present ? setD3dxKey(text, section, key, saved.value)
                             : removeD3dxKey(text, section, key);
    };
    std::string out = restoreOne(iniText, "System", "load_library_redirect", backup.loadLibraryRedirect);
    out = restoreOne(out, "System", "allow_platform_update", backup.allowPlatformUpdate);
    out = restoreOne(out, "Device", "upscaling", backup.upscaling);
    return out;
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
