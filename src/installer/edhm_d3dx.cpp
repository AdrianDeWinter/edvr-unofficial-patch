#include "edhm_d3dx.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

#include "update_util.h"

namespace edvr::installer {
namespace {

constexpr size_t kMaxIncludeDepth = 16;
constexpr size_t kMaxIncludeFiles = 2048;
constexpr size_t kMaxIncludeDirs = 512;
constexpr unsigned long long kMaxIncludedIniBytes = 4ULL * 1024ULL * 1024ULL;
constexpr unsigned long long kMaxTotalIncludedBytes = 32ULL * 1024ULL * 1024ULL;

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isHorizSpace(char c) {
    return c == ' ' || c == '\t';
}

char asciiLowerChar(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string asciiLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = asciiLowerChar(c);
    return out;
}

std::wstring wideLower(const std::wstring& s) {
    std::wstring out = s;
    if (!out.empty()) CharLowerBuffW(&out[0], static_cast<DWORD>(out.size()));
    return out;
}

bool asciiEq(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (asciiLowerChar(a[i]) != asciiLowerChar(b[i])) return false;
    }
    return true;
}

bool asciiStartsWith(const std::string& a, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (a.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (asciiLowerChar(a[i]) != asciiLowerChar(prefix[i])) return false;
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

std::string trimHoriz(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && isHorizSpace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && isHorizSpace(s[b - 1])) --b;
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

// Single section-header parser matching 2DMigoto's ParseIniSectionLine (IniHandler.cpp:469-476):
// any line whose first non-horizontal-whitespace char is '[' begins a section, taking text up to
// the first ']' (or end of line if no ']') and trimming horizontal whitespace inside.
bool parseMigotoSectionHeader(const std::string& body, std::string* sectionName) {
    const std::string trimmed = trimHoriz(body);
    if (trimmed.empty() || trimmed[0] != '[') return false;
    size_t close = trimmed.find(']', 1);
    if (close == std::string::npos) close = trimmed.size();
    *sectionName = trimHoriz(trimmed.substr(1, close - 1));
    return true;
}

struct ParsedKeyLine {
    bool isKey = false;
    bool commented = false;
    std::string key;
    std::string value;
    std::string prefix;        // everything up to and including '=' and spaces after '='
    std::string inlineComment; // whitespace + ';...' at end of line, only when stripInlineComment is true
};

ParsedKeyLine parseKeyLine(const std::string& body, bool stripInlineComment = true) {
    ParsedKeyLine out;
    size_t i = 0;
    while (i < body.size() && isHorizSpace(body[i])) ++i;
    if (i >= body.size()) return out;

    bool commented = false;
    if (body[i] == ';') {
        commented = true;
        ++i;
        while (i < body.size() && (isHorizSpace(body[i]) || body[i] == ';')) ++i;
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
    while (valStart < body.size() && isHorizSpace(body[valStart])) ++valStart;

    size_t commentPos = (stripInlineComment && !commented) ? body.find(';', valStart) : std::string::npos;
    size_t valEnd = commentPos == std::string::npos ? body.size() : commentPos;
    while (valEnd > valStart && isHorizSpace(body[valEnd - 1])) --valEnd;

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

D3dxKeyRead readD3dxKeyInternal(const std::string& iniText, const char* section, const char* key,
                                bool stripInlineComment) {
    D3dxKeyRead result;
    if (isUtf16D3dx(iniText)) return result;
    bool hasBom = false;
    std::string defaultEol;
    const std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    bool inFirstSection = false;
    bool foundSection = false;
    for (const RawLine& line : lines) {
        std::string sec;
        if (parseMigotoSectionHeader(line.body, &sec)) {
            if (inFirstSection) break;
            if (!foundSection && !sec.empty() && asciiEq(sec, section)) {
                inFirstSection = true;
                foundSection = true;
            }
            continue;
        }
        if (!inFirstSection) continue;
        const ParsedKeyLine parsed = parseKeyLine(line.body, stripInlineComment);
        if (parsed.isKey && !parsed.commented && !parsed.value.empty() && asciiEq(parsed.key, key)) {
            result.present = true;
            result.value = parsed.value;
            return result;
        }
    }
    return result;
}

std::string setD3dxKeyInternal(const std::string& iniText, const char* section, const char* key,
                               const std::string& value, bool stripInlineComment) {
    if (isUtf16D3dx(iniText)) return iniText;
    bool hasBom = false;
    std::string defaultEol;
    std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    size_t firstActiveNonEmptyMatch = static_cast<size_t>(-1);
    ParsedKeyLine firstActiveNonEmptyParsed;
    size_t firstActiveEmptyMatch = static_cast<size_t>(-1);
    ParsedKeyLine firstActiveEmptyParsed;
    size_t firstCommentedMatch = static_cast<size_t>(-1);
    size_t firstSectionEnd = static_cast<size_t>(-1);
    bool inFirstSection = false;
    bool foundSection = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sec;
        if (parseMigotoSectionHeader(lines[i].body, &sec)) {
            if (inFirstSection) {
                firstSectionEnd = i;
                break;
            }
            if (!foundSection && !sec.empty() && asciiEq(sec, section)) {
                inFirstSection = true;
                foundSection = true;
            }
            continue;
        }
        if (!inFirstSection) continue;
        const ParsedKeyLine parsed = parseKeyLine(lines[i].body, stripInlineComment);
        if (parsed.isKey && asciiEq(parsed.key, key)) {
            if (!parsed.commented) {
                if (!parsed.value.empty()) {
                    if (firstActiveNonEmptyMatch == static_cast<size_t>(-1)) {
                        firstActiveNonEmptyMatch = i;
                        firstActiveNonEmptyParsed = parsed;
                    }
                } else if (firstActiveEmptyMatch == static_cast<size_t>(-1)) {
                    firstActiveEmptyMatch = i;
                    firstActiveEmptyParsed = parsed;
                }
            } else if (firstCommentedMatch == static_cast<size_t>(-1)) {
                firstCommentedMatch = i;
            }
        }
    }
    if (inFirstSection && firstSectionEnd == static_cast<size_t>(-1)) firstSectionEnd = lines.size();

    if (firstActiveNonEmptyMatch != static_cast<size_t>(-1)) {
        lines[firstActiveNonEmptyMatch].body =
            firstActiveNonEmptyParsed.prefix + value + firstActiveNonEmptyParsed.inlineComment;
        return joinLines(lines, hasBom);
    }
    if (firstActiveEmptyMatch != static_cast<size_t>(-1)) {
        lines[firstActiveEmptyMatch].body =
            firstActiveEmptyParsed.prefix + value + firstActiveEmptyParsed.inlineComment;
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

std::string removeD3dxKeyInternal(const std::string& iniText, const char* section, const char* key,
                                  bool stripInlineComment) {
    if (isUtf16D3dx(iniText)) return iniText;
    bool hasBom = false;
    std::string defaultEol;
    std::vector<RawLine> lines = splitLines(iniText, &hasBom, &defaultEol);

    bool inFirstSection = false;
    bool foundSection = false;
    size_t firstActiveNonEmpty = static_cast<size_t>(-1);
    size_t firstActiveEmpty = static_cast<size_t>(-1);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sec;
        if (parseMigotoSectionHeader(lines[i].body, &sec)) {
            if (inFirstSection) break;
            if (!foundSection && !sec.empty() && asciiEq(sec, section)) {
                inFirstSection = true;
                foundSection = true;
            }
            continue;
        }
        if (!inFirstSection) continue;
        const ParsedKeyLine parsed = parseKeyLine(lines[i].body, stripInlineComment);
        if (parsed.isKey && !parsed.commented && asciiEq(parsed.key, key)) {
            if (!parsed.value.empty()) {
                firstActiveNonEmpty = i;
                break;
            }
            if (firstActiveEmpty == static_cast<size_t>(-1)) firstActiveEmpty = i;
        }
    }
    const size_t target = (firstActiveNonEmpty != static_cast<size_t>(-1)) ? firstActiveNonEmpty : firstActiveEmpty;
    if (target == static_cast<size_t>(-1)) return iniText;
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(target));
    return joinLines(lines, hasBom);
}

// Recognize only a single scalar token and its optional sign. EDHM_2DMigoto's
// CommandList.cpp ParseFloatToken treats ERANGE as infinity even on underflow.
std::optional<float> readScalarLiteral(const std::string& raw) {
    const std::string value = asciiLower(raw);
    if (value.empty()) return std::nullopt;
    const bool negative = value[0] == '-';
    const char* token = value.c_str() + (negative || value[0] == '+');
    if (std::strncmp(token, "0b", 2) == 0) {
        const size_t digits = std::strlen(token + 2);
        if (digits == 0 || digits > 64) return std::nullopt;
        uint64_t bits = 0;
        for (const char* digit = token + 2; *digit; ++digit) {
            if (*digit != '0' && *digit != '1') return std::nullopt;
            bits = (bits << 1) | static_cast<uint64_t>(*digit - '0');
        }
        const float number = static_cast<float>(bits);
        return negative ? -number : number;
    }
    if (!(*token >= '0' && *token <= '9') && *token != '.' && std::strcmp(token, "inf") != 0 &&
        std::strcmp(token, "infinity") != 0 && std::strcmp(token, "nan") != 0) {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    float number = std::strtof(token, &end);
    if (end == token || *end != '\0') return std::nullopt;
    if (errno == ERANGE) number = std::copysign(std::numeric_limits<float>::infinity(), number);
    return negative ? -number : number;
}

std::optional<bool> evalAllowPlatformUpdate(const D3dxKeyRead& key) {
    // A full-line comment never reaches here. A mid-line ';' makes the fork's
    // value parser fall back to its default (true, or zero for upscaling).
    if (!key.present || key.value.find(';') != std::string::npos) return true;
    const std::string& value = key.value;
    if (asciiEq(value, "1") || asciiEq(value, "true") || asciiEq(value, "yes") || asciiEq(value, "on")) return true;
    if (asciiEq(value, "0") || asciiEq(value, "false") || asciiEq(value, "no") || asciiEq(value, "off")) return false;
    const auto number = readScalarLiteral(value);
    if (!number) return std::nullopt;
    return !std::isnan(*number) && *number != 0.0f;
}

std::optional<bool> evalUpscalingOk(const D3dxKeyRead& key) {
    if (!key.present || key.value.find(';') != std::string::npos) return true;
    // ParseIntValue tries a decimal integer before using a float token. Neither
    // integer saturation nor float truncation changes whether the result is zero
    // beyond the checks below (ConvertExpressionToInt maps NaN to zero).
    const char* token = key.value.c_str();
    char* end = nullptr;
    const long long integer = std::strtoll(token, &end, 10);
    if (end != token && *end == '\0') return integer == 0;
    const auto number = readScalarLiteral(key.value);
    if (!number) return std::nullopt;
    return std::isnan(*number) || (*number > -1.0f && *number < 1.0f);
}

struct WStringInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

struct AsciiInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    }
};

// Case-insensitive glob matching (* and ?) for [Include] exclude_recursive patterns
// against a single directory or file basename (IniHandler.cpp:890-913, 945).
bool globMatchInsensitive(const std::wstring& pattern, const std::wstring& text) {
    size_t pi = 0;
    size_t ti = 0;
    size_t starPi = std::wstring::npos;
    size_t starTi = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() &&
            (pattern[pi] == L'?' ||
             towlower(static_cast<wint_t>(pattern[pi])) == towlower(static_cast<wint_t>(text[ti])))) {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == L'*') {
            starPi = pi++;
            starTi = ti;
        } else if (starPi != std::wstring::npos) {
            pi = starPi + 1;
            ti = ++starTi;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == L'*') ++pi;
    return pi == pattern.size();
}

bool matchesExcludePatterns(const wchar_t* name, const std::vector<std::wstring>& patterns) {
    const std::wstring text(name);
    for (const std::wstring& pat : patterns) {
        if (globMatchInsensitive(pat, text)) return true;
    }
    return false;
}

bool isWindowsReservedDeviceName(const std::wstring& seg) {
    const size_t dot = seg.find(L'.');
    std::wstring base = seg.substr(0, dot);
    while (!base.empty() && base.back() == L' ') base.pop_back();
    if (_wcsicmp(base.c_str(), L"CON") == 0 || _wcsicmp(base.c_str(), L"PRN") == 0 ||
        _wcsicmp(base.c_str(), L"AUX") == 0 || _wcsicmp(base.c_str(), L"NUL") == 0 ||
        _wcsicmp(base.c_str(), L"CONIN$") == 0 || _wcsicmp(base.c_str(), L"CONOUT$") == 0) {
        return true;
    }
    if (base.size() == 4 && std::wcschr(L"123456789\u00b9\u00b2\u00b3", base[3])) {
        if (_wcsnicmp(base.c_str(), L"COM", 3) == 0 || _wcsnicmp(base.c_str(), L"LPT", 3) == 0) {
            return true;
        }
    }
    return false;
}

bool missingPath(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Normalizes `rawRel` relative to `gameDir` and verifies strict containment inside `gameDir`:
// rejects drive letters, UNC/absolute roots, escaping '..', embedded ':' or NUL, trailing space/dot
// segments (Win32 strips them, which could normalize ".. " to ".."), DOS device names, and reparse
// points along the relative chain. When `allowRoot` is true (for `include_recursive = .`), a path
// resolving to `gameDir` itself is accepted with empty `*normRelOut`.
const char* resolveContainedRelativePath(const std::wstring& gameDir, const std::wstring& rawRel, bool allowRoot,
                                         std::wstring* fullOut, std::wstring* normRelOut) {
    constexpr const char* unsafe = "unsafe include path";
    if (rawRel.empty() || rawRel[0] == L'\\' || rawRel[0] == L'/') return unsafe;

    std::vector<std::wstring> segs;
    size_t pos = 0;
    while (pos <= rawRel.size()) {
        size_t sep = rawRel.find_first_of(L"\\/", pos);
        const size_t end = (sep == std::wstring::npos) ? rawRel.size() : sep;
        const std::wstring seg = rawRel.substr(pos, end - pos);
        if (!seg.empty() && seg != L".") {
            if (seg == L"..") {
                if (segs.empty()) return unsafe;
                segs.pop_back();
            } else {
                if (seg.find_first_of(L":<>\"|?*") != std::wstring::npos ||
                    std::any_of(seg.begin(), seg.end(), [](wchar_t c) { return c < L' '; }) ||
                    seg.back() == L' ' || seg.back() == L'.' || isWindowsReservedDeviceName(seg)) {
                    return unsafe;
                }
                segs.push_back(seg);
            }
        }
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }
    if (segs.empty() && !allowRoot) return unsafe;

    std::wstring normRel;
    std::wstring walk = gameDir;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i > 0) normRel += L'\\';
        normRel += segs[i];
        walk = joinPath(walk, segs[i]);
        const DWORD attrs = GetFileAttributesW(walk.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (!missingPath(GetLastError())) return "cannot read include path attributes";
        } else if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return "include path contains a reparse point";
        }
    }
    *fullOut = walk;
    *normRelOut = normRel;
    return nullptr;
}

// Opens a regular, non-reparse disk file without following a leaf symlink/junction and reads up to
// `maxBytes` directly from that handle (avoiding leaf TOCTOU and DOS device opens).
const char* readRegularDiskFileBytes(const std::wstring& path, unsigned long long maxBytes, const char* limit,
                                     bool fileLimitReached, std::optional<std::string>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // The fork skips a missing include or user_config. Unexpected open
        // errors leave the effective settings unknown.
        return missingPath(GetLastError()) ? nullptr : "cannot open included config for reading";
    }
    struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() { CloseHandle(handle); }
    } guard{h};

    if (GetFileType(h) != FILE_TYPE_DISK) return "included config is not a regular disk file";
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return "cannot read included config attributes";
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return "included config is a reparse point";
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return "included config is a directory";
    if (fileLimitReached) return "included config file count limit exceeded";
    const unsigned long long fileSize =
        (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (fileSize > maxBytes) return limit;

    std::string buf;
    buf.reserve(static_cast<size_t>(fileSize));
    char chunk[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, chunk, sizeof(chunk), &got, nullptr)) return "cannot read included config";
        if (got == 0) break;
        if (buf.size() + got > maxBytes) return limit;
        buf.append(chunk, got);
    }
    *out = std::move(buf);
    return nullptr;
}

std::wstring parentPrefixOf(const std::wstring& relPath) {
    const size_t slash = relPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return relPath.substr(0, slash + 1);
}

struct PendingIncludeSection {
    std::string sectionSortKey;
    std::wstring dirPrefix;
    std::vector<std::pair<std::string, std::string>> entries;
};

struct EffectiveState {
    D3dxKeyRead allowPlatformUpdate;
    std::string allowPlatformUpdateSource;  // empty = main d3dx.ini
    D3dxKeyRead upscaling;
    std::string upscalingSource;            // empty = main d3dx.ini

    bool hasUserConfig = false;
    std::string userConfig = "d3dx_user.ini";
    std::vector<std::wstring> excludePatterns;

    std::set<std::wstring> seenFiles;  // canonical lowercased normRel of included files already parsed
    std::set<std::wstring> seenDirs;   // canonical lowercased normRel of recursive include dirs already walked
    size_t parsedFiles = 0;
    size_t walkedDirs = 0;
    unsigned long long totalIncludedBytes = 0;
    std::string incompleteReason;

    void reportIncomplete(const std::wstring& path, const char* reason) {
        if (!incompleteReason.empty()) return;
        incompleteReason = (path.empty() ? "d3dx.ini" : narrowUtf8(path)) + ": " + reason;
    }
};

// Parses one INI file with 2DMigoto's exact line/section/key rules (IniHandler.cpp:454-749):
// - No mid-line ';' stripping.
// - Empty values are ignored (IniHandler.cpp:637-645).
// - Root file (`!isIncluded`): first section & first non-empty key win.
// - Included/user file (`isIncluded`): preamble `condition = <expr>` can skip the file;
//   global [System]/[Device] sections allow duplicates and overwrite last-write-wins.
void parseMigotoIniText(const std::string& text, bool isIncluded, const std::wstring& relPath,
                        EffectiveState* state, std::vector<PendingIncludeSection>* outIncludes) {
    if (isUtf16D3dx(text)) {
        state->reportIncomplete(relPath, "UTF-16 or embedded NUL bytes");
        return;
    }
    if (!text.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0) == 0) {
        state->reportIncomplete(relPath, "invalid UTF-8 encoding");
        return;
    }
    bool hasBom = false;
    std::string defaultEol;
    const std::vector<RawLine> lines = splitLines(text, &hasBom, &defaultEol);

    const std::string sourceUtf8 = isIncluded ? narrowUtf8(relPath) : std::string();
    const std::wstring dirPrefix = isIncluded ? parentPrefixOf(relPath) : std::wstring();

    std::unordered_set<std::string> seenSectionsInFile;
    std::string currentSection;
    bool sectionActive = false;
    bool preamble = true;
    size_t activeIncludeIndex = static_cast<size_t>(-1);
    std::string namespaceName = sourceUtf8;

    for (const RawLine& raw : lines) {
        const std::string line = trimHoriz(raw.body);
        if (line.empty() || line[0] == ';') continue;

        if (line[0] == '[') {
            preamble = false;
            activeIncludeIndex = static_cast<size_t>(-1);
            std::string sec;
            if (!parseMigotoSectionHeader(line, &sec) || sec.empty()) {
                currentSection.clear();
                sectionActive = false;
                continue;
            }
            const std::string secLower = asciiLower(sec);
            const bool isGlobalCompat = (secLower == "system" || secLower == "device");
            const bool isIncludeSec = asciiStartsWith(sec, "include");
            if (!isIncluded) {
                if (!seenSectionsInFile.insert(secLower).second) {
                    currentSection.clear();
                    sectionActive = false;
                    continue;
                }
            } else if (!isGlobalCompat) {
                // Included [Include*] sections are namespaced per file (IniHandler.cpp:130, 484),
                // so duplicates within the same file are ignored while global sections merge/override.
                if (!seenSectionsInFile.insert(secLower).second) {
                    currentSection.clear();
                    sectionActive = false;
                    continue;
                }
            }
            currentSection = secLower;
            sectionActive = true;
            if (isIncludeSec && outIncludes) {
                PendingIncludeSection pending;
                if (!isIncluded) {
                    pending.sectionSortKey = sec;
                } else {
                    // Matches get_namespaced_section_name (IniHandler.cpp:357): Include\<ns>\<suffix>
                    pending.sectionSortKey = "Include\\" + namespaceName + "\\" + sec.substr(7);
                }
                pending.dirPrefix = dirPrefix;
                activeIncludeIndex = outIncludes->size();
                outIncludes->push_back(std::move(pending));
            }
            continue;
        }

        const size_t eq = line.find('=');
        if (preamble) {
            if (eq != std::string::npos) {
                const std::string pkey = trimHoriz(line.substr(0, eq));
                const std::string pval = trimHoriz(line.substr(eq + 1));
                if (asciiEq(pkey, "condition")) {
                    const auto condition = readScalarLiteral(pval);
                    if (!condition) {
                        state->reportIncomplete(relPath, "preamble condition needs expression evaluation");
                        return;
                    }
                    if (*condition == 0.0f) return;
                } else if (asciiEq(pkey, "namespace") && !pval.empty()) {
                    // Renames section namespace while keeping dirPrefix tied to ini_path
                    // (IniHandler.cpp:526-527, 410-414, 602-607).
                    namespaceName = pval;
                }
            }
            continue;
        }

        if (!sectionActive || eq == std::string::npos) continue;
        const std::string key = trimHoriz(line.substr(0, eq));
        const std::string val = trimHoriz(line.substr(eq + 1));
        // Empty value is skipped by ParseIniKeyValLine (IniHandler.cpp:637-645).
        if (key.empty() || val.empty()) continue;

        if (currentSection == "system" && asciiEq(key, "allow_platform_update")) {
            if (!isIncluded) {
                if (!state->allowPlatformUpdate.present) {
                    state->allowPlatformUpdate.present = true;
                    state->allowPlatformUpdate.value = val;
                    state->allowPlatformUpdateSource.clear();
                }
            } else {
                state->allowPlatformUpdate.present = true;
                state->allowPlatformUpdate.value = val;
                state->allowPlatformUpdateSource = sourceUtf8;
            }
        } else if (currentSection == "device" && asciiEq(key, "upscaling")) {
            if (!isIncluded) {
                if (!state->upscaling.present) {
                    state->upscaling.present = true;
                    state->upscaling.value = val;
                    state->upscalingSource.clear();
                }
            } else {
                state->upscaling.present = true;
                state->upscaling.value = val;
                state->upscalingSource = sourceUtf8;
            }
        } else if (activeIncludeIndex != static_cast<size_t>(-1) && outIncludes) {
            if (!isIncluded && currentSection == "include") {
                if (asciiEq(key, "user_config") && !state->hasUserConfig) {
                    state->hasUserConfig = true;
                    state->userConfig = val;
                } else if (asciiEq(key, "exclude_recursive")) {
                    state->excludePatterns.push_back(widenUtf8(val));
                }
            }
            (*outIncludes)[activeIncludeIndex].entries.emplace_back(key, val);
        }
    }
}

void loadSingleIncludedIni(const std::wstring& gameDir, const std::wstring& rawRel, bool isUserConfig,
                           EffectiveState* state, std::vector<PendingIncludeSection>* nextIncludes) {
    std::wstring fullPath;
    std::wstring normRel;
    if (const char* error = resolveContainedRelativePath(gameDir, rawRel, false, &fullPath, &normRel)) {
        state->reportIncomplete(rawRel, error);
        return;
    }
    if (!isUserConfig && !state->seenFiles.insert(wideLower(normRel)).second) return;
    const unsigned long long budget =
        isUserConfig ? kMaxIncludedIniBytes
                     : (std::min)(kMaxIncludedIniBytes, kMaxTotalIncludedBytes - state->totalIncludedBytes);
    const char* limit = budget < kMaxIncludedIniBytes ? "total included config byte limit exceeded"
                                                    : "included config file size limit exceeded";
    std::optional<std::string> bytes;
    const bool fileLimitReached = !isUserConfig && state->parsedFiles >= kMaxIncludeFiles;
    if (const char* error = readRegularDiskFileBytes(fullPath, budget, limit, fileLimitReached, &bytes)) {
        state->reportIncomplete(normRel, error);
        return;
    }
    if (!bytes) return;
    if (!isUserConfig) {
        ++state->parsedFiles;
        state->totalIncludedBytes += bytes->size();
    }
    parseMigotoIniText(*bytes, true, normRel, state, nextIncludes);
}

void loadIncludedIniRecursive(const std::wstring& gameDir, const std::wstring& rawRel, size_t depth,
                              EffectiveState* state, std::vector<PendingIncludeSection>* nextIncludes) {
    std::wstring dirPath;
    std::wstring normDirRel;
    if (const char* error = resolveContainedRelativePath(gameDir, rawRel, true, &dirPath, &normDirRel)) {
        state->reportIncomplete(rawRel, error);
        return;
    }
    if (!state->seenDirs.insert(wideLower(normDirRel)).second) return;
    const DWORD dirAttrs = GetFileAttributesW(dirPath.c_str());
    if (dirAttrs == INVALID_FILE_ATTRIBUTES) {
        if (!missingPath(GetLastError())) state->reportIncomplete(rawRel, "cannot access recursive include directory");
        return;
    }
    if ((dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        state->reportIncomplete(rawRel, "recursive include directory is a reparse point");
        return;
    }
    if ((dirAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return;  // The fork's "\\*" finds no files here.
    if (depth > kMaxIncludeDepth) {
        state->reportIncomplete(rawRel, "recursive include depth limit exceeded");
        return;
    }
    if (state->walkedDirs >= kMaxIncludeDirs) {
        state->reportIncomplete(rawRel, "recursive include directory count limit exceeded");
        return;
    }
    ++state->walkedDirs;

    std::set<std::wstring, WStringInsensitiveLess> iniFiles;
    std::set<std::wstring, WStringInsensitiveLess> subDirs;
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(joinPath(dirPath, L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            state->reportIncomplete(rawRel, "cannot enumerate recursive include directory");
        }
        return;
    }
    do {
        if (matchesExcludePatterns(fd.cFileName, state->excludePatterns)) continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (std::wcscmp(fd.cFileName, L".") != 0 && std::wcscmp(fd.cFileName, L"..") != 0) {
                subDirs.insert(fd.cFileName);
            }
        } else {
            const size_t len = std::wcslen(fd.cFileName);
            // Matches ParseIniFilesRecursive (IniHandler.cpp:956): exact lowercase ".ini" suffix.
            if (len >= 4 && std::wcscmp(fd.cFileName + len - 4, L".ini") == 0) {
                iniFiles.insert(fd.cFileName);
            }
        }
    } while (FindNextFileW(hFind, &fd));
    const DWORD enumerationError = GetLastError();
    FindClose(hFind);
    if (enumerationError != ERROR_NO_MORE_FILES) {
        state->reportIncomplete(rawRel, "error while enumerating recursive include directory");
        return;
    }

    for (const std::wstring& leaf : iniFiles) {
        const std::wstring childRel = normDirRel.empty() ? leaf : (normDirRel + L"\\" + leaf);
        // Never re-parse root d3dx.ini as an included override when include_recursive = .
        if (normDirRel.empty() && _wcsicmp(leaf.c_str(), L"d3dx.ini") == 0) continue;
        loadSingleIncludedIni(gameDir, childRel, false, state, nextIncludes);
        if (!state->incompleteReason.empty()) return;
    }
    for (const std::wstring& sub : subDirs) {
        const std::wstring childDir = normDirRel.empty() ? sub : (normDirRel + L"\\" + sub);
        loadIncludedIniRecursive(gameDir, childDir, depth + 1, state, nextIncludes);
        if (!state->incompleteReason.empty()) return;
    }
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
    return readD3dxKeyInternal(iniText, section, key, true);
}

std::string setD3dxKey(const std::string& iniText, const char* section, const char* key,
                       const std::string& value) {
    return setD3dxKeyInternal(iniText, section, key, value, true);
}

std::string removeD3dxKey(const std::string& iniText, const char* section, const char* key) {
    return removeD3dxKeyInternal(iniText, section, key, true);
}

EdhmD3dxEffectiveCompat evaluateEdhmD3dxCompat(const std::wstring& gameDir, const std::string& d3dxIniText) {
    EffectiveState state;
    std::vector<PendingIncludeSection> pending;
    parseMigotoIniText(d3dxIniText, false, std::wstring(), &state, &pending);

    if (!gameDir.empty()) {
        // Breadth-first rounds matching ParseIncludedIniFiles (IniHandler.cpp:1726-1790).
        while (!pending.empty() && state.incompleteReason.empty()) {
            std::sort(pending.begin(), pending.end(),
                      [](const PendingIncludeSection& a, const PendingIncludeSection& b) {
                          return AsciiInsensitiveLess{}(a.sectionSortKey, b.sectionSortKey);
                      });
            std::vector<PendingIncludeSection> nextRound;
            for (const PendingIncludeSection& sec : pending) {
                for (const auto& kv : sec.entries) {
                    if (!state.incompleteReason.empty()) break;
                    // Matches IniHandler.cpp:1766-1774 (exact lowercase key names).
                    if (kv.first != "include" && kv.first != "include_recursive") continue;
                    const std::wstring rawRel = sec.dirPrefix + widenUtf8(kv.second);
                    if (kv.first == "include") {
                        loadSingleIncludedIni(gameDir, rawRel, false, &state, &nextRound);
                    } else {
                        loadIncludedIniRecursive(gameDir, rawRel, 0, &state, &nextRound);
                    }
                }
                if (!state.incompleteReason.empty()) break;
            }
            pending = std::move(nextRound);
        }

        if (state.incompleteReason.empty()) {
            // The fork loads user_config last, independent of ordinary includes.
            const std::wstring userConfig = widenUtf8(state.userConfig);
            if (std::any_of(userConfig.begin(), userConfig.end(), [](wchar_t c) { return c > 127; })) {
                // GetUserConfigPath widens UTF-8 bytes individually (IniHandler.cpp:1690).
                state.reportIncomplete(userConfig, "unsupported non-ASCII user_config path");
            } else {
                loadSingleIncludedIni(gameDir, userConfig, true, &state, nullptr);
            }
        }
    }

    EdhmD3dxEffectiveCompat out;
    const auto allow = evalAllowPlatformUpdate(state.allowPlatformUpdate);
    const auto upscaling = evalUpscalingOk(state.upscaling);
    if (!allow) {
        state.reportIncomplete(widenUtf8(state.allowPlatformUpdateSource),
                               "[System] allow_platform_update needs expression evaluation");
    }
    if (!upscaling) {
        state.reportIncomplete(widenUtf8(state.upscalingSource), "[Device] upscaling needs expression evaluation");
    }
    out.incompleteReason = std::move(state.incompleteReason);
    if (!out.complete()) {
        out.satisfied = false;
        return out;
    }
    out.allowPlatformUpdateOk = *allow;
    out.upscalingOk = *upscaling;
    if (!out.allowPlatformUpdateOk && !state.allowPlatformUpdateSource.empty()) {
        out.allowPlatformUpdateOverrideFile = state.allowPlatformUpdateSource;
    }
    if (!out.upscalingOk && !state.upscalingSource.empty()) {
        out.upscalingOverrideFile = state.upscalingSource;
    }
    out.satisfied = out.allowPlatformUpdateOk && out.upscalingOk;
    return out;
}

bool edhmD3dxCompatSatisfied(const std::string& iniText) {
    return evaluateEdhmD3dxCompat(std::wstring(), iniText).satisfied;
}

std::string describeEdhmD3dxOverrideFailure(const EdhmD3dxEffectiveCompat& compat) {
    std::string out;
    if (!compat.allowPlatformUpdateOk && !compat.allowPlatformUpdateOverrideFile.empty()) {
        out += "[System] allow_platform_update in " + compat.allowPlatformUpdateOverrideFile;
    }
    if (!compat.upscalingOk && !compat.upscalingOverrideFile.empty()) {
        if (!out.empty()) out += " and ";
        out += "[Device] upscaling in " + compat.upscalingOverrideFile;
    }
    return out;
}

EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxEffectiveCompat& effective) {
    EdhmD3dxBackup b;
    if (!effective.complete() || isUtf16D3dx(iniText)) return b;
    EffectiveState rootState;
    parseMigotoIniText(iniText, false, std::wstring(), &rootState, nullptr);
    if (!effective.allowPlatformUpdateOk && effective.allowPlatformUpdateOverrideFile.empty() &&
        rootState.allowPlatformUpdate.present) {
        b.allowPlatformUpdateSaved = true;
        b.allowPlatformUpdate.present = true;
        b.allowPlatformUpdate.value = rootState.allowPlatformUpdate.value;
    }
    if (!effective.upscalingOk && effective.upscalingOverrideFile.empty() &&
        rootState.upscaling.present) {
        b.upscalingSaved = true;
        b.upscaling.present = true;
        b.upscaling.value = rootState.upscaling.value;
    }
    return b;
}

EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText) {
    return snapshotEdhmD3dxCompat(iniText, evaluateEdhmD3dxCompat(std::wstring(), iniText));
}

std::string applyEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxEffectiveCompat& effective) {
    if (!effective.complete() || isUtf16D3dx(iniText)) return iniText;
    std::string out = iniText;
    if (!effective.allowPlatformUpdateOk && effective.allowPlatformUpdateOverrideFile.empty()) {
        out = setD3dxKeyInternal(out, "System", "allow_platform_update", "1", false);
    }
    if (!effective.upscalingOk && effective.upscalingOverrideFile.empty()) {
        out = setD3dxKeyInternal(out, "Device", "upscaling", "0", false);
    }
    return out;
}

std::string applyEdhmD3dxCompat(const std::string& iniText) {
    return applyEdhmD3dxCompat(iniText, evaluateEdhmD3dxCompat(std::wstring(), iniText));
}

std::string describeEdhmD3dxBackup(const EdhmD3dxBackup& backup) {
    std::string out;
    auto add = [&](const char* section, const char* key, const EdhmD3dxKeyBackup& saved) {
        if (!out.empty()) out += ", ";
        out += std::string("[") + section + "] " + key;
        if (saved.present) {
            out += " = " + saved.value;
        } else {
            out += " (originally absent)";
        }
    };
    if (backup.loadLibraryRedirectSaved) add("System", "load_library_redirect", backup.loadLibraryRedirect);
    if (backup.allowPlatformUpdateSaved) add("System", "allow_platform_update", backup.allowPlatformUpdate);
    if (backup.upscalingSaved) add("Device", "upscaling", backup.upscaling);
    if (backup.loggingReloadSaved) add("Logging", "show_reload_message", backup.showReloadMessage);
    if (backup.loggingWarningsSaved) add("Logging", "show_warnings", backup.showWarnings);
    return out;
}

std::string restoreEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxBackup& backup) {
    if (!backup.anySaved() || isUtf16D3dx(iniText)) return iniText;
    auto restoreIfStillOurs = [](const std::string& text, const char* section, const char* key,
                                 const EdhmD3dxKeyBackup& saved, const char* expectedEdupValue,
                                 bool stripInlineComment) {
        const D3dxKeyRead cur = readD3dxKeyInternal(text, section, key, stripInlineComment);
        if (!cur.present || cur.value != expectedEdupValue) return text;
        return saved.present ? setD3dxKeyInternal(text, section, key, saved.value, stripInlineComment)
                             : removeD3dxKeyInternal(text, section, key, stripInlineComment);
    };
    std::string out = iniText;
    if (backup.loadLibraryRedirectSaved) {
        // Legacy #250 builds preserved inline comments when writing load_library_redirect = 0.
        out = restoreIfStillOurs(out, "System", "load_library_redirect", backup.loadLibraryRedirect, "0", true);
    }
    if (backup.allowPlatformUpdateSaved) {
        out = restoreIfStillOurs(out, "System", "allow_platform_update", backup.allowPlatformUpdate, "1", false);
    }
    if (backup.upscalingSaved) {
        out = restoreIfStillOurs(out, "Device", "upscaling", backup.upscaling, "0", false);
    }
    if (backup.loggingReloadSaved) {
        out = restoreIfStillOurs(out, "Logging", "show_reload_message", backup.showReloadMessage, "0", true);
    }
    if (backup.loggingWarningsSaved) {
        out = restoreIfStillOurs(out, "Logging", "show_warnings", backup.showWarnings, "0", true);
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
