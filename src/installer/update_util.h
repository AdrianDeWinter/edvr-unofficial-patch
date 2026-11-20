// Small helpers the update code shares: hex checks, UTF-8 conversion and a few
// file operations. Nothing here knows about manifests or the network.
#pragma once

#include <string>

namespace edvr::installer {

// True when `text` is exactly `digits` characters of 0-9 and a-f. Upper case is
// not accepted: every hash and name the update code handles is written lower
// case, and one spelling is one cache file.
bool isLowerHex(const std::string& text, size_t digits);

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
