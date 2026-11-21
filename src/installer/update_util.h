// Small helpers the update code shares: hex checks, UTF-8 conversion and a few
// file operations. Nothing here knows about manifests or the network.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

// True when `text` is exactly `digits` characters of 0-9 and a-f. Upper case is
// not accepted: every hash and name the update code handles is written lower
// case, and one spelling is one cache file.
bool isLowerHex(const std::string& text, size_t digits);

// Encodes `bytes` as lower- or upper-case hexadecimal (two characters per byte).
std::string hexLower(const unsigned char* bytes, size_t size);
std::string hexUpper(const unsigned char* bytes, size_t size);

// Decodes an even-length hexadecimal string (accepts both cases unless `lowerOnly` is true).
// Returns false on empty input, odd length, or any non-hex character.
bool decodeHex(const std::string& text, std::vector<unsigned char>* out, bool lowerOnly = false);

// Case-insensitive non-empty path comparison.
bool samePath(const std::wstring& a, const std::wstring& b);

std::wstring widenUtf8(const std::string& text);
std::string narrowUtf8(const std::wstring& text);

// Reads a whole file, refusing one longer than `maxBytes`. `error` (may be
// null) gets a short ASCII sentence without the path.
bool readFileBytes(const std::wstring& path, std::string* out, unsigned long long maxBytes,
                   std::string* error);

// CreateDirectory for every missing level; true when the folder exists after.
bool createDirectories(const std::wstring& path);

// a + "\" + b, without doubling a separator. Defined in detect.cpp, where the installer has
// always had it: the launcher links both files, and two definitions would not link.
std::wstring joinPath(const std::wstring& base, const std::wstring& leaf);

}  // namespace edvr::installer
