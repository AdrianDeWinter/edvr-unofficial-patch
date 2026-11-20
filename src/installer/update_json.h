// A small strict JSON parser for the two documents the launcher reads: the
// release manifest and GitHub's release API reply.
//
// Strict means: nesting deeper than 32 is refused, bytes after the value are
// refused, an object that repeats a key is refused, a string must be valid
// UTF-8 with no raw control character, numbers follow the JSON grammar (no
// leading zeros, no NaN). \u escapes are decoded to UTF-8; a surrogate pair
// becomes one character and half of a pair is kept as its own three bytes, as
// Python's json does. An integer that does not fit in 64
// bits is refused rather than rounded, so a size or a timestamp is never a
// different number from the one written.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

inline constexpr int kJsonMaxDepth = 32;

struct JsonValue {
    enum class Type { Null, Bool, Int, Float, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    long long integer = 0;          // Int
    double number = 0;              // Float
    std::string text;               // String, decoded to UTF-8
    std::vector<JsonValue> items;   // Array elements, or Object values
    std::vector<std::string> keys;  // Object keys, parallel to items

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isInt() const { return type == Type::Int; }

    // The value of `key` in an object, or null when this is no object or lacks it.
    const JsonValue* find(const std::string& key) const;
    // The same, ignoring ASCII case. For documents whose key spelling the launcher did not choose.
    const JsonValue* findNoCase(const std::string& key) const;
};

// Parses all of `text`. On failure `error` (may be null) gets a sentence with
// the byte offset, and `out` is left empty.
bool parseJson(const std::string& text, JsonValue* out, std::string* error);

}  // namespace edvr::installer
