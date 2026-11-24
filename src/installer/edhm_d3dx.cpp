#include "edhm_d3dx.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
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

// Clamped float -> int conversion matching ConvertExpressionToInt (IniHandler.cpp:1359-1372)
// without undefined behavior on NaN, infinities, or out-of-range floats.
int safeFloatToInt(float expr) {
    if (std::isnan(expr)) return 0;
    if (expr <= static_cast<float>(INT_MIN)) return INT_MIN;
    if (expr >= static_cast<float>(INT_MAX)) return INT_MAX;
    return static_cast<int>(expr);
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

// Constant-expression evaluator mirroring EDHM_2DMigoto's CommandListExpression
// static evaluation (DirectX11/CommandList.cpp:4067-4097, 4532-4750, 5226-5272)
// and IniHandler.cpp:1204-1281 (ParseBinaryLiterals, ParseIniExpression).
// Supports decimal/hex/binary/inf/nan literals, parentheses, unary + - ! ~, and
// binary arithmetic/exponent/shift/comparison/logical/bitwise operators at
// CommandListExpression precedence. Any syntax error (such as a mid-line ';') or
// unknown identifier returns false so callers fall back to default.
class StaticExprParser {
public:
    explicit StaticExprParser(const std::string& input) : s_(asciiLower(input)) {}

    bool parse(float* out) {
        pos_ = 0;
        depth_ = 0;
        float v = 0.0f;
        if (!parseOr(&v)) return false;
        skipWs();
        if (pos_ != s_.size()) return false;
        *out = v;
        return true;
    }

private:
    void skipWs() {
        while (pos_ < s_.size() && isHorizSpace(s_[pos_])) ++pos_;
    }

    bool match(const char* tok) {
        skipWs();
        const size_t n = std::strlen(tok);
        if (s_.size() - pos_ < n || s_.compare(pos_, n, tok) != 0) return false;
        pos_ += n;
        return true;
    }

    bool peekIs(const char* tok) {
        skipWs();
        const size_t n = std::strlen(tok);
        return s_.size() - pos_ >= n && s_.compare(pos_, n, tok) == 0;
    }

    static float safeDiv(float a, float b) {
        if (b == 0.0f) {
            if (a == 0.0f || std::isnan(a)) return std::numeric_limits<float>::quiet_NaN();
            const bool neg = std::signbit(a) != std::signbit(b);
            return neg ? -std::numeric_limits<float>::infinity()
                       : std::numeric_limits<float>::infinity();
        }
        return a / b;
    }

    bool parseOr(float* out) {
        if (!parseAnd(out)) return false;
        while (match("||")) {
            float rhs = 0.0f;
            if (!parseAnd(&rhs)) return false;
            *out = ((*out != 0.0f && !std::isnan(*out)) || (rhs != 0.0f && !std::isnan(rhs))) ? 1.0f : 0.0f;
        }
        return true;
    }

    bool parseAnd(float* out) {
        if (!parseBitOr(out)) return false;
        while (match("&&")) {
            float rhs = 0.0f;
            if (!parseBitOr(&rhs)) return false;
            *out = ((*out != 0.0f && !std::isnan(*out)) && (rhs != 0.0f && !std::isnan(rhs))) ? 1.0f : 0.0f;
        }
        return true;
    }

    bool parseBitOr(float* out) {
        if (!parseBitXor(out)) return false;
        for (;;) {
            if (peekIs("||") || !match("|")) break;
            float rhs = 0.0f;
            if (!parseBitXor(&rhs)) return false;
            const uint32_t a = static_cast<uint32_t>(safeFloatToInt(*out));
            const uint32_t b = static_cast<uint32_t>(safeFloatToInt(rhs));
            *out = static_cast<float>(static_cast<int32_t>(a | b));
        }
        return true;
    }

    bool parseBitXor(float* out) {
        if (!parseBitAnd(out)) return false;
        while (match("^")) {
            float rhs = 0.0f;
            if (!parseBitAnd(&rhs)) return false;
            const uint32_t a = static_cast<uint32_t>(safeFloatToInt(*out));
            const uint32_t b = static_cast<uint32_t>(safeFloatToInt(rhs));
            *out = static_cast<float>(static_cast<int32_t>(a ^ b));
        }
        return true;
    }

    bool parseBitAnd(float* out) {
        if (!parseEq(out)) return false;
        for (;;) {
            if (peekIs("&&") || !match("&")) break;
            float rhs = 0.0f;
            if (!parseEq(&rhs)) return false;
            const uint32_t a = static_cast<uint32_t>(safeFloatToInt(*out));
            const uint32_t b = static_cast<uint32_t>(safeFloatToInt(rhs));
            *out = static_cast<float>(static_cast<int32_t>(a & b));
        }
        return true;
    }

    bool parseEq(float* out) {
        if (!parseRel(out)) return false;
        for (;;) {
            if (match("===")) {
                float rhs = 0.0f;
                if (!parseRel(&rhs)) return false;
                const bool eq = (std::isnan(*out) && std::isnan(rhs)) ||
                                (std::memcmp(out, &rhs, sizeof(float)) == 0);
                *out = eq ? 1.0f : 0.0f;
            } else if (match("!==")) {
                float rhs = 0.0f;
                if (!parseRel(&rhs)) return false;
                const bool eq = (std::isnan(*out) && std::isnan(rhs)) ||
                                (std::memcmp(out, &rhs, sizeof(float)) == 0);
                *out = eq ? 0.0f : 1.0f;
            } else if (match("==")) {
                float rhs = 0.0f;
                if (!parseRel(&rhs)) return false;
                *out = (*out == rhs) ? 1.0f : 0.0f;
            } else if (match("!=")) {
                float rhs = 0.0f;
                if (!parseRel(&rhs)) return false;
                *out = (*out != rhs) ? 1.0f : 0.0f;
            } else {
                break;
            }
        }
        return true;
    }

    bool parseRel(float* out) {
        if (!parseShift(out)) return false;
        for (;;) {
            if (match("<=")) {
                float rhs = 0.0f;
                if (!parseShift(&rhs)) return false;
                *out = (*out <= rhs) ? 1.0f : 0.0f;
            } else if (match(">=")) {
                float rhs = 0.0f;
                if (!parseShift(&rhs)) return false;
                *out = (*out >= rhs) ? 1.0f : 0.0f;
            } else if (!peekIs("<<") && match("<")) {
                float rhs = 0.0f;
                if (!parseShift(&rhs)) return false;
                *out = (*out < rhs) ? 1.0f : 0.0f;
            } else if (!peekIs(">>") && match(">")) {
                float rhs = 0.0f;
                if (!parseShift(&rhs)) return false;
                *out = (*out > rhs) ? 1.0f : 0.0f;
            } else {
                break;
            }
        }
        return true;
    }

    bool parseShift(float* out) {
        if (!parseAdd(out)) return false;
        for (;;) {
            if (match("<<")) {
                float rhs = 0.0f;
                if (!parseAdd(&rhs)) return false;
                const int shift = safeFloatToInt(rhs);
                const uint32_t a = static_cast<uint32_t>(safeFloatToInt(*out));
                const uint32_t r = (shift >= 0 && shift < 32) ? (a << static_cast<unsigned>(shift)) : 0u;
                *out = static_cast<float>(static_cast<int32_t>(r));
            } else if (match(">>")) {
                float rhs = 0.0f;
                if (!parseAdd(&rhs)) return false;
                const int shift = safeFloatToInt(rhs);
                const int32_t a = static_cast<int32_t>(safeFloatToInt(*out));
                int32_t r = 0;
                if (shift >= 0 && shift < 32) {
                    r = (a >= 0) ? static_cast<int32_t>(static_cast<uint32_t>(a) >> static_cast<unsigned>(shift))
                                 : ~static_cast<int32_t>(static_cast<uint32_t>(~a) >> static_cast<unsigned>(shift));
                } else if (shift >= 32 && a < 0) {
                    r = -1;
                }
                *out = static_cast<float>(r);
            } else {
                break;
            }
        }
        return true;
    }

    bool parseAdd(float* out) {
        if (!parseMul(out)) return false;
        for (;;) {
            if (match("+")) {
                float rhs = 0.0f;
                if (!parseMul(&rhs)) return false;
                *out += rhs;
            } else if (match("-")) {
                float rhs = 0.0f;
                if (!parseMul(&rhs)) return false;
                *out -= rhs;
            } else {
                break;
            }
        }
        return true;
    }

    bool parseMul(float* out) {
        if (!parsePow(out)) return false;
        for (;;) {
            if (!peekIs("**") && match("*")) {
                float rhs = 0.0f;
                if (!parsePow(&rhs)) return false;
                *out *= rhs;
            } else if (match("//")) {
                float rhs = 0.0f;
                if (!parsePow(&rhs)) return false;
                *out = std::floor(safeDiv(*out, rhs));
            } else if (match("/")) {
                float rhs = 0.0f;
                if (!parsePow(&rhs)) return false;
                *out = safeDiv(*out, rhs);
            } else if (match("%")) {
                float rhs = 0.0f;
                if (!parsePow(&rhs)) return false;
                *out = (rhs == 0.0f) ? std::numeric_limits<float>::quiet_NaN() : std::fmod(*out, rhs);
            } else {
                break;
            }
        }
        return true;
    }

    // Right-associative exponentiation (`**`, CommandList.cpp:5237).
    bool parsePow(float* out) {
        if (!parseUnary(out)) return false;
        if (match("**")) {
            if (++depth_ > 64) return false;
            struct DepthGuard {
                int* d;
                ~DepthGuard() { --(*d); }
            } guard{&depth_};
            float rhs = 0.0f;
            if (!parsePow(&rhs)) return false;
            *out = std::pow(*out, rhs);
        }
        return true;
    }

    bool parseUnary(float* out) {
        skipWs();
        if (++depth_ > 64) return false;
        struct DepthGuard {
            int* d;
            ~DepthGuard() { --(*d); }
        } guard{&depth_};

        if (match("+")) return parseUnary(out);
        if (match("-")) {
            float v = 0.0f;
            if (!parseUnary(&v)) return false;
            *out = -v;
            return true;
        }
        if (match("!")) {
            float v = 0.0f;
            if (!parseUnary(&v)) return false;
            *out = (v == 0.0f || std::isnan(v)) ? 1.0f : 0.0f;
            return true;
        }
        if (match("~")) {
            float v = 0.0f;
            if (!parseUnary(&v)) return false;
            const uint32_t bits = ~static_cast<uint32_t>(safeFloatToInt(v));
            *out = static_cast<float>(static_cast<int32_t>(bits));
            return true;
        }
        return parsePrimary(out);
    }

    bool parsePrimary(float* out) {
        skipWs();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == '(') {
            ++pos_;
            if (!parseOr(out)) return false;
            return match(")");
        }
        // Binary literal: 0b[01]+ up to 64 bits (CommandList.cpp:4070-4079, IniHandler.cpp:1204-1231).
        if (s_.size() - pos_ >= 3 && s_[pos_] == '0' && s_[pos_ + 1] == 'b') {
            size_t p = pos_ + 2;
            uint64_t val = 0;
            size_t bits = 0;
            while (p < s_.size() && (s_[p] == '0' || s_[p] == '1')) {
                if (bits >= 64) return false;
                val = (val << 1) | static_cast<uint64_t>(s_[p] - '0');
                ++bits;
                ++p;
            }
            if (bits == 0) return false;
            if (p < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p])) || s_[p] == '.')) return false;
            pos_ = p;
            *out = static_cast<float>(val);
            return true;
        }
        // Hex literal: 0x[0-9a-f]+ (without hex float '.'/'p' suffix handled by strtof below).
        if (s_.size() - pos_ >= 3 && s_[pos_] == '0' && s_[pos_ + 1] == 'x') {
            size_t p = pos_ + 2;
            while (p < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[p]))) ++p;
            if (p > pos_ + 2 && (p == s_.size() || (s_[p] != '.' && s_[p] != 'p'))) {
                if (p < s_.size() && std::isalnum(static_cast<unsigned char>(s_[p]))) return false;
                errno = 0;
                char* end = nullptr;
                const unsigned long long v = std::strtoull(s_.c_str() + pos_, &end, 16);
                if (end != s_.c_str() + p) return false;
                pos_ = p;
                *out = (errno == ERANGE) ? std::numeric_limits<float>::infinity() : static_cast<float>(v);
                return true;
            }
        }
        // Decimal / hex-float / special literals (inf, infinity, nan) matching ParseFloatValue /
        // ParseFloatToken (IniHandler.cpp:1335-1344, CommandList.cpp:4081-4096).
        const char c = s_[pos_];
        const bool startsNum = std::isdigit(static_cast<unsigned char>(c)) || c == '.';
        const bool startsSpecial =
            (s_.compare(pos_, 3, "inf") == 0) || (s_.compare(pos_, 3, "nan") == 0);
        if (!startsNum && !startsSpecial) return false;

        errno = 0;
        char* end = nullptr;
        const float v = std::strtof(s_.c_str() + pos_, &end);
        if (end == s_.c_str() + pos_) return false;
        pos_ = static_cast<size_t>(end - s_.c_str());
        // Overflow only (IniHandler.cpp:1337-1342): underflow (e.g. 1e-50) stays 0.0f / subnormal.
        if (errno == ERANGE && std::isinf(v)) {
            *out = std::signbit(v) ? -std::numeric_limits<float>::infinity()
                                   : std::numeric_limits<float>::infinity();
        } else {
            *out = v;
        }
        return true;
    }

    std::string s_;
    size_t pos_ = 0;
    int depth_ = 0;
};

bool evalD3dxStaticExpr(const std::string& raw, float* out) {
    StaticExprParser parser(raw);
    return parser.parse(out);
}

// Matches EDHM_2DMigoto DirectX11/IniHandler.cpp:1420-1449, 5051
// (GetIniBool(L"System", L"allow_platform_update", true, nullptr)).
bool evalAllowPlatformUpdate(const D3dxKeyRead& key) {
    if (!key.present) return true;
    const std::string t = trim(key.value);
    if (t.empty()) return true;
    if (asciiEq(t, "1") || asciiEq(t, "true") || asciiEq(t, "yes") || asciiEq(t, "on")) return true;
    if (asciiEq(t, "0") || asciiEq(t, "false") || asciiEq(t, "no") || asciiEq(t, "off")) return false;
    float expr = 0.0f;
    if (!evalD3dxStaticExpr(t, &expr)) return true;  // parse failure -> default true
    return !std::isnan(expr) && expr != 0.0f;
}

// Matches EDHM_2DMigoto DirectX11/IniHandler.cpp:1359-1411, 5141
// (GetIniInt(L"Device", L"upscaling", 0, nullptr)).
bool evalUpscalingOk(const D3dxKeyRead& key) {
    if (!key.present) return true;
    const std::string t = trim(key.value);
    if (t.empty()) return true;
    // Fast path: base-10 integer (IniHandler.cpp:1379-1398).
    {
        errno = 0;
        char* end = nullptr;
        const long long n = std::strtoll(t.c_str(), &end, 10);
        if (end != t.c_str() && *end == '\0') {
            int v = 0;
            if (errno == ERANGE) {
                v = (n < 0) ? INT_MIN : INT_MAX;
            } else if (n < INT_MIN) {
                v = INT_MIN;
            } else if (n > INT_MAX) {
                v = INT_MAX;
            } else {
                v = static_cast<int>(n);
            }
            return v == 0;
        }
    }
    float expr = 0.0f;
    if (!evalD3dxStaticExpr(t, &expr)) return true;  // parse failure -> default 0
    return safeFloatToInt(expr) == 0;
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
    const std::wstring base = (dot == std::wstring::npos) ? seg : seg.substr(0, dot);
    if (base.size() < 3 || base.size() > 4) return false;
    if (_wcsicmp(base.c_str(), L"CON") == 0 || _wcsicmp(base.c_str(), L"PRN") == 0 ||
        _wcsicmp(base.c_str(), L"AUX") == 0 || _wcsicmp(base.c_str(), L"NUL") == 0) {
        return true;
    }
    if (base.size() == 4 && base[3] >= L'1' && base[3] <= L'9') {
        if (_wcsnicmp(base.c_str(), L"COM", 3) == 0 || _wcsnicmp(base.c_str(), L"LPT", 3) == 0) {
            return true;
        }
    }
    return false;
}

// Normalizes `rawRel` relative to `gameDir` and verifies strict containment inside `gameDir`:
// rejects drive letters, UNC/absolute roots, escaping '..', embedded ':' or NUL, trailing space/dot
// segments (Win32 strips them, which could normalize ".. " to ".."), DOS device names, and reparse
// points along the relative chain. When `allowRoot` is true (for `include_recursive = .`), a path
// resolving to `gameDir` itself is accepted with empty `*normRelOut`.
bool resolveContainedRelativePath(const std::wstring& gameDir, const std::wstring& rawRel, bool allowRoot,
                                  std::wstring* fullOut, std::wstring* normRelOut) {
    if (gameDir.empty() || rawRel.empty()) return false;
    if (rawRel[0] == L'\\' || rawRel[0] == L'/') return false;
    if (rawRel.size() >= 2 && rawRel[1] == L':') return false;

    std::vector<std::wstring> segs;
    size_t pos = 0;
    while (pos <= rawRel.size()) {
        size_t sep = rawRel.find_first_of(L"\\/", pos);
        const size_t end = (sep == std::wstring::npos) ? rawRel.size() : sep;
        const std::wstring seg = rawRel.substr(pos, end - pos);
        if (!seg.empty() && seg != L".") {
            if (seg == L"..") {
                if (segs.empty()) return false;
                segs.pop_back();
            } else {
                if (seg.find(L':') != std::wstring::npos || seg.find(L'\0') != std::wstring::npos) return false;
                if (seg.back() == L' ' || seg.back() == L'.') return false;
                if (isWindowsReservedDeviceName(seg)) return false;
                segs.push_back(seg);
            }
        }
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }
    if (segs.empty()) {
        if (!allowRoot) return false;
        *fullOut = gameDir;
        normRelOut->clear();
        return true;
    }

    std::wstring normRel;
    std::wstring walk = gameDir;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i > 0) normRel += L'\\';
        normRel += segs[i];
        walk = joinPath(walk, segs[i]);
        const DWORD attrs = GetFileAttributesW(walk.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
    }
    *fullOut = walk;
    *normRelOut = normRel;
    return true;
}

// Opens a regular, non-reparse disk file without following a leaf symlink/junction and reads up to
// `maxBytes` directly from that handle (avoiding leaf TOCTOU and DOS device opens).
bool readRegularDiskFileBytes(const std::wstring& path, unsigned long long maxBytes, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() { CloseHandle(handle); }
    } guard{h};

    if (GetFileType(h) != FILE_TYPE_DISK) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return false;
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) return false;
    const unsigned long long fileSize =
        (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (fileSize > maxBytes) return false;

    std::string buf;
    buf.reserve(static_cast<size_t>(fileSize));
    char chunk[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, chunk, sizeof(chunk), &got, nullptr)) return false;
        if (got == 0) break;
        if (buf.size() + got > maxBytes) return false;
        buf.append(chunk, got);
    }
    *out = std::move(buf);
    return true;
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
};

// Parses one INI file with 2DMigoto's exact line/section/key rules (IniHandler.cpp:454-749):
// - No mid-line ';' stripping.
// - Empty values are ignored (IniHandler.cpp:637-645).
// - Root file (`!isIncluded`): first section & first non-empty key win.
// - Included/user file (`isIncluded`): preamble `condition = <expr>` can skip the file;
//   global [System]/[Device] sections allow duplicates and overwrite last-write-wins.
void parseMigotoIniText(const std::string& text, bool isIncluded, const std::wstring& relPath,
                        EffectiveState* state, std::vector<PendingIncludeSection>* outIncludes) {
    if (isUtf16D3dx(text)) return;
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
            if (isIncluded && eq != std::string::npos) {
                const std::string pkey = trimHoriz(line.substr(0, eq));
                const std::string pval = trimHoriz(line.substr(eq + 1));
                if (asciiEq(pkey, "condition")) {
                    // Matches check_include_condition (IniHandler.cpp:548-574):
                    // parse failure or static value 0 skips the entire included file.
                    float condVal = 0.0f;
                    if (!evalD3dxStaticExpr(pval, &condVal) || condVal == 0.0f) return;
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
    if (!isUserConfig &&
        (state->parsedFiles >= kMaxIncludeFiles || state->totalIncludedBytes >= kMaxTotalIncludedBytes)) {
        return;
    }
    std::wstring fullPath;
    std::wstring normRel;
    if (!resolveContainedRelativePath(gameDir, rawRel, false, &fullPath, &normRel)) return;
    if (!isUserConfig) {
        if (!state->seenFiles.insert(wideLower(normRel)).second) return;
    }
    const unsigned long long budget =
        isUserConfig ? kMaxIncludedIniBytes
                     : (std::min)(kMaxIncludedIniBytes, kMaxTotalIncludedBytes - state->totalIncludedBytes);
    std::string bytes;
    if (!readRegularDiskFileBytes(fullPath, budget, &bytes)) return;
    if (isUtf16D3dx(bytes)) return;
    if (!isUserConfig) {
        ++state->parsedFiles;
        state->totalIncludedBytes += bytes.size();
    }
    parseMigotoIniText(bytes, true, normRel, state, nextIncludes);
}

void loadIncludedIniRecursive(const std::wstring& gameDir, const std::wstring& rawRel, size_t depth,
                              EffectiveState* state, std::vector<PendingIncludeSection>* nextIncludes) {
    if (depth > kMaxIncludeDepth || state->parsedFiles >= kMaxIncludeFiles ||
        state->walkedDirs >= kMaxIncludeDirs || state->totalIncludedBytes >= kMaxTotalIncludedBytes) {
        return;
    }
    std::wstring dirPath;
    std::wstring normDirRel;
    if (!resolveContainedRelativePath(gameDir, rawRel, true, &dirPath, &normDirRel)) return;
    if (!state->seenDirs.insert(wideLower(normDirRel)).second) return;
    const DWORD dirAttrs = GetFileAttributesW(dirPath.c_str());
    if (dirAttrs == INVALID_FILE_ATTRIBUTES || (dirAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return;
    ++state->walkedDirs;

    std::set<std::wstring, WStringInsensitiveLess> iniFiles;
    std::set<std::wstring, WStringInsensitiveLess> subDirs;
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(joinPath(dirPath, L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (matchesExcludePatterns(fd.cFileName, state->excludePatterns)) continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
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
    FindClose(hFind);

    for (const std::wstring& leaf : iniFiles) {
        if (state->parsedFiles >= kMaxIncludeFiles || state->totalIncludedBytes >= kMaxTotalIncludedBytes) return;
        const std::wstring childRel = normDirRel.empty() ? leaf : (normDirRel + L"\\" + leaf);
        // Never re-parse root d3dx.ini as an included override when include_recursive = .
        if (normDirRel.empty() && _wcsicmp(leaf.c_str(), L"d3dx.ini") == 0) continue;
        loadSingleIncludedIni(gameDir, childRel, false, state, nextIncludes);
    }
    for (const std::wstring& sub : subDirs) {
        if (state->parsedFiles >= kMaxIncludeFiles || state->walkedDirs >= kMaxIncludeDirs ||
            state->totalIncludedBytes >= kMaxTotalIncludedBytes) {
            return;
        }
        const std::wstring childDir = normDirRel.empty() ? sub : (normDirRel + L"\\" + sub);
        loadIncludedIniRecursive(gameDir, childDir, depth + 1, state, nextIncludes);
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
    EdhmD3dxEffectiveCompat out;
    if (isUtf16D3dx(d3dxIniText)) {
        out.satisfied = false;
        out.allowPlatformUpdateOk = false;
        out.upscalingOk = false;
        return out;
    }

    EffectiveState state;
    std::vector<PendingIncludeSection> pending;
    parseMigotoIniText(d3dxIniText, false, std::wstring(), &state, &pending);

    if (!gameDir.empty()) {
        // Breadth-first rounds matching ParseIncludedIniFiles (IniHandler.cpp:1726-1790).
        while (!pending.empty() && state.parsedFiles < kMaxIncludeFiles &&
               state.totalIncludedBytes < kMaxTotalIncludedBytes) {
            std::sort(pending.begin(), pending.end(),
                      [](const PendingIncludeSection& a, const PendingIncludeSection& b) {
                          return AsciiInsensitiveLess{}(a.sectionSortKey, b.sectionSortKey);
                      });
            std::vector<PendingIncludeSection> nextRound;
            for (const PendingIncludeSection& sec : pending) {
                for (const auto& kv : sec.entries) {
                    if (state.parsedFiles >= kMaxIncludeFiles ||
                        state.totalIncludedBytes >= kMaxTotalIncludedBytes) {
                        break;
                    }
                    // Matches IniHandler.cpp:1766-1774 (exact lowercase key names).
                    if (kv.first != "include" && kv.first != "include_recursive") continue;
                    const std::wstring rawRel = sec.dirPrefix + widenUtf8(kv.second);
                    if (kv.first == "include") {
                        loadSingleIncludedIni(gameDir, rawRel, false, &state, &nextRound);
                    } else {
                        loadIncludedIniRecursive(gameDir, rawRel, 0, &state, &nextRound);
                    }
                }
            }
            pending = std::move(nextRound);
        }

        // Loaded last outside the include file cap so user_config (default d3dx_user.ini)
        // always overrides root and ordinary includes (IniHandler.cpp:1682-1696, 5027-5034).
        const std::string userCfg = state.userConfig.empty() ? "d3dx_user.ini" : state.userConfig;
        loadSingleIncludedIni(gameDir, widenUtf8(userCfg), true, &state, nullptr);
    }

    out.allowPlatformUpdateOk = evalAllowPlatformUpdate(state.allowPlatformUpdate);
    out.upscalingOk = evalUpscalingOk(state.upscaling);
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
    if (isUtf16D3dx(iniText)) return b;
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
    if (isUtf16D3dx(iniText)) return iniText;
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
