#include "update_util.h"

#include <windows.h>

#include <vector>

namespace edvr::installer {

bool isLowerHex(const std::string& text, size_t digits) {
    if (text.size() != digits) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

namespace {

std::string encodeHex(const unsigned char* bytes, size_t size, const char* digits) {
    if (!bytes || size == 0) return std::string();
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

int hexNibble(char c, bool lowerOnly) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (!lowerOnly && c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string hexLower(const unsigned char* bytes, size_t size) { return encodeHex(bytes, size, "0123456789abcdef"); }

std::string hexUpper(const unsigned char* bytes, size_t size) { return encodeHex(bytes, size, "0123456789ABCDEF"); }

bool decodeHex(const std::string& text, std::vector<unsigned char>* out, bool lowerOnly) {
    if (out) out->clear();
    if (text.empty() || text.size() % 2 != 0) return false;
    std::vector<unsigned char> decoded;
    decoded.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexNibble(text[i], lowerOnly);
        const int lo = hexNibble(text[i + 1], lowerOnly);
        if (hi < 0 || lo < 0) return false;
        decoded.push_back(static_cast<unsigned char>(hi * 16 + lo));
    }
    if (out) *out = std::move(decoded);
    return true;
}

bool samePath(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string narrowUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

bool readFileBytes(const std::wstring& path, std::string* out, unsigned long long maxBytes,
                   std::string* error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (error) *error = "the file cannot be opened";
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        CloseHandle(f);
        if (error) *error = "the file is larger than allowed";
        return false;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t done = 0;
    while (done < bytes.size()) {
        const size_t left = bytes.size() - done;
        const DWORD want = static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left);
        DWORD got = 0;
        if (!ReadFile(f, &bytes[done], want, &got, nullptr) || got == 0) {
            CloseHandle(f);
            if (error) *error = "the file cannot be read";
            return false;
        }
        done += got;
    }
    CloseHandle(f);
    *out = std::move(bytes);
    return true;
}

bool createDirectories(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0) {
        const std::wstring parent = path.substr(0, cut);
        // "C:" alone names a drive's current folder, not something to create.
        if (parent.size() > 2 || parent.back() != L':') {
            if (!createDirectories(parent)) return false;
        }
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace edvr::installer
