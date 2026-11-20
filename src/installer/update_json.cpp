#include "update_json.h"

#include <cstdlib>
#include <utility>

namespace edvr::installer {

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

const JsonValue* JsonValue::findNoCase(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    auto same = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
            const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
            if (x != y) return false;
        }
        return true;
    };
    for (size_t i = 0; i < keys.size(); ++i) {
        if (same(keys[i], key)) return &items[i];
    }
    return nullptr;
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool run(JsonValue* out) {
        skipSpace();
        if (!value(out, 0)) return false;
        skipSpace();
        if (pos_ != s_.size()) return fail("bytes follow the value");
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool fail(const char* what) {
        if (error_.empty()) error_ = std::string(what) + " at byte " + std::to_string(pos_);
        return false;
    }

    void skipSpace() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool word(const char* literal) {
        size_t i = 0;
        while (literal[i]) {
            if (pos_ + i >= s_.size() || s_[pos_ + i] != literal[i]) return false;
            ++i;
        }
        pos_ += i;
        return true;
    }

    bool value(JsonValue* v, int depth) {
        if (pos_ >= s_.size()) return fail("the text ends early");
        const char c = s_[pos_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') {
            v->type = JsonValue::Type::String;
            return string(&v->text);
        }
        if (c == 't') {
            if (!word("true")) return fail("unexpected character");
            v->type = JsonValue::Type::Bool;
            v->boolean = true;
            return true;
        }
        if (c == 'f') {
            if (!word("false")) return fail("unexpected character");
            v->type = JsonValue::Type::Bool;
            v->boolean = false;
            return true;
        }
        if (c == 'n') {
            if (!word("null")) return fail("unexpected character");
            v->type = JsonValue::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return numberValue(v);
        return fail("unexpected character");
    }

    bool array(JsonValue* v, int depth) {
        if (depth + 1 > kJsonMaxDepth) return fail("nesting is too deep");
        v->type = JsonValue::Type::Array;
        ++pos_;
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            JsonValue element;
            skipSpace();
            if (!value(&element, depth + 1)) return false;
            v->items.push_back(std::move(element));
            skipSpace();
            if (pos_ >= s_.size()) return fail("the text ends early");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected a comma or ]");
        }
    }

    bool object(JsonValue* v, int depth) {
        if (depth + 1 > kJsonMaxDepth) return fail("nesting is too deep");
        v->type = JsonValue::Type::Object;
        ++pos_;
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a key");
            std::string key;
            if (!string(&key)) return false;
            for (const std::string& seen : v->keys) {
                if (seen == key) return fail("a key is repeated");
            }
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected a colon");
            ++pos_;
            skipSpace();
            JsonValue element;
            if (!value(&element, depth + 1)) return false;
            v->keys.push_back(std::move(key));
            v->items.push_back(std::move(element));
            skipSpace();
            if (pos_ >= s_.size()) return fail("the text ends early");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected a comma or }");
        }
    }

    bool numberValue(JsonValue* v) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) return fail("a number ends early");
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return fail("a number has no digits");
        }
        bool isFloat = false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            isFloat = true;
            ++pos_;
            const size_t digits = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return fail("a number has no digits after the point");
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            isFloat = true;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            const size_t digits = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return fail("a number has no exponent digits");
        }
        const std::string literal = s_.substr(start, pos_ - start);
        if (isFloat) {
            v->type = JsonValue::Type::Float;
            v->number = std::strtod(literal.c_str(), nullptr);
            return true;
        }
        // An integer, with its own overflow check, so "-0" is 0 and 2^63 is refused.
        const bool negative = literal[0] == '-';
        unsigned long long magnitude = 0;
        for (size_t i = negative ? 1 : 0; i < literal.size(); ++i) {
            const unsigned digit = static_cast<unsigned>(literal[i] - '0');
            if (magnitude > (0xffffffffffffffffull - digit) / 10) {
                pos_ = start;
                return fail("an integer is too large");
            }
            magnitude = magnitude * 10 + digit;
        }
        const unsigned long long limit = negative ? 0x8000000000000000ull : 0x7fffffffffffffffull;
        if (magnitude > limit) {
            pos_ = start;
            return fail("an integer is too large");
        }
        v->type = JsonValue::Type::Int;
        v->integer = negative ? static_cast<long long>(0ull - magnitude) : static_cast<long long>(magnitude);
        return true;
    }

    static void appendUtf8(std::string* out, unsigned long cp) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned long* out) {
        if (pos_ + 4 > s_.size()) return fail("a \\u escape ends early");
        unsigned long value = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char c = s_[pos_ + i];
            unsigned long digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned long>(c - 'A' + 10);
            else return fail("a \\u escape has a non-hex digit");
            value = value * 16 + digit;
        }
        pos_ += 4;
        *out = value;
        return true;
    }

    // One UTF-8 sequence starting at pos_, validated and copied. Overlong forms,
    // surrogates and anything past U+10FFFF are refused.
    bool copyUtf8(std::string* out) {
        const unsigned char lead = static_cast<unsigned char>(s_[pos_]);
        size_t extra;
        unsigned long cp;
        unsigned long minimum;
        if (lead >= 0xC2 && lead <= 0xDF) { extra = 1; cp = lead & 0x1Fu; minimum = 0x80; }
        else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; cp = lead & 0x0Fu; minimum = 0x800; }
        else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; cp = lead & 0x07u; minimum = 0x10000; }
        else return fail("a string holds invalid UTF-8");
        if (pos_ + extra >= s_.size()) return fail("a string holds invalid UTF-8");
        for (size_t i = 1; i <= extra; ++i) {
            const unsigned char next = static_cast<unsigned char>(s_[pos_ + i]);
            if ((next & 0xC0) != 0x80) return fail("a string holds invalid UTF-8");
            cp = (cp << 6) | (next & 0x3Fu);
        }
        if (cp < minimum || cp > 1114111 || (cp >= 0xD800 && cp <= 0xDFFF))
            return fail("a string holds invalid UTF-8");
        out->append(s_, pos_, extra + 1);
        pos_ += extra + 1;
        return true;
    }

    bool string(std::string* out) {
        ++pos_;  // the opening quote
        out->clear();
        for (;;) {
            if (pos_ >= s_.size()) return fail("a string is not closed");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return fail("a string holds a raw control character");
            if (c >= 0x80) {
                if (!copyUtf8(out)) return false;
                continue;
            }
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= s_.size()) return fail("a string is not closed");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!hex4(&cp)) return false;
                    // A pair joins into one code point. Half of a pair is kept as the
                    // three bytes its value would take, as Python's json does with the
                    // same text, so the two validators give one verdict on a manifest
                    // that carries one; no field that matters accepts a byte above 0x7e.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        const size_t resume = pos_;
                        pos_ += 2;
                        unsigned long low = 0;
                        if (hex4(&low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            if (!error_.empty()) return false;
                            pos_ = resume;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("a string has an unknown escape");
            }
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace

bool parseJson(const std::string& text, JsonValue* out, std::string* error) {
    Parser parser(text);
    JsonValue value;
    if (!parser.run(&value)) {
        if (error) *error = parser.error();
        *out = JsonValue();
        return false;
    }
    *out = std::move(value);
    return true;
}

}  // namespace edvr::installer
