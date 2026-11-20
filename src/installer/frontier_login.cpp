#include "frontier_login.h"

#include <windows.h>
#include <wincrypt.h>

#include <vector>

#include "../common/iniedit.h"
#include "update_util.h"

#pragma comment(lib, "crypt32.lib")

namespace edvr::installer {
namespace {

constexpr const char kHeader[] = "EDUP-FRONTIER-LOGIN 1";
// This program's own, so a blob protected here does not open for code that merely calls DPAPI.
constexpr char kEntropy[] = "ED-UP launcher / Frontier login / v1";

std::string toHex(const unsigned char* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

bool fromHex(const std::string& text, std::vector<unsigned char>* out) {
    out->clear();
    if (text.empty() || text.size() % 2 != 0) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = digit(text[i]), lo = digit(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<unsigned char>(hi * 16 + lo));
    }
    return true;
}

DATA_BLOB entropyBlob() {
    DATA_BLOB blob;
    blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    blob.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
    return blob;
}

bool protect(const std::string& plain, std::string* hex) {
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ED-UP Frontier login", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    *hex = toHex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool unprotect(const std::string& hex, std::string* plain) {
    std::vector<unsigned char> bytes;
    if (!fromHex(hex, &bytes)) return false;
    DATA_BLOB in;
    in.pbData = bytes.data();
    in.cbData = static_cast<DWORD>(bytes.size());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    plain->assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool hasLineBreak(const std::string& text) { return text.find_first_of("\r\n") != std::string::npos; }

}  // namespace

void wipeString(std::string* text) {
    if (!text->empty()) SecureZeroMemory(&(*text)[0], text->size());
    text->clear();
}

bool loadFrontierLogin(const std::wstring& path, StoredLogin* out) {
    *out = StoredLogin();
    std::string bytes;
    if (path.empty() || !readFileBytes(path, &bytes, 1u << 16, nullptr)) return false;

    StoredLogin found;
    // Every way out wipes what was decrypted here; on success *out holds the only copy.
    auto discard = [&]() {
        wipeString(&found.password);
        wipeString(&found.machineToken);
        return false;
    };
    bool headerSeen = false, emailSeen = false;
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        if (end == std::string::npos) end = bytes.size();
        std::string line = bytes.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;
        if (line.empty()) continue;
        if (!headerSeen) {
            if (line != kHeader) return discard();
            headerSeen = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return discard();
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "email") {
            found.email = value;
            emailSeen = true;
        } else if (key == "password") {
            if (!unprotect(value, &found.password)) return discard();
        } else if (key == "machine_token") {
            if (!unprotect(value, &found.machineToken)) return discard();
        } else {
            return discard();  // a key this version did not write: not a file to trust
        }
    }
    if (!headerSeen || !emailSeen || found.email.empty()) return discard();
    *out = found;
    discard();
    return true;
}

bool saveFrontierLogin(const std::wstring& path, const StoredLogin& login, std::string* error) {
    if (path.empty() || login.email.empty() || hasLineBreak(login.email)) {
        if (error) *error = "the login cannot be saved";
        return false;
    }
    std::string text = std::string(kHeader) + "\n" + "email=" + login.email + "\n";
    std::string hex;
    if (!login.password.empty()) {
        if (!protect(login.password, &hex)) {
            if (error) *error = "Windows could not encrypt the login";
            return false;
        }
        text += "password=" + hex + "\n";
    }
    if (!login.machineToken.empty()) {
        if (!protect(login.machineToken, &hex)) {
            if (error) *error = "Windows could not encrypt the login";
            return false;
        }
        text += "machine_token=" + hex + "\n";
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) createDirectories(path.substr(0, slash));
    std::wstring why;
    if (!edvr::writeFileAtomic(path, text, &why)) {
        if (error) *error = "the login could not be written";
        return false;
    }
    return true;
}

bool forgetFrontierSecrets(const std::wstring& path) {
    StoredLogin login;
    if (!loadFrontierLogin(path, &login)) return deleteFrontierLogin(path);
    wipeString(&login.password);
    wipeString(&login.machineToken);
    return saveFrontierLogin(path, login, nullptr);
}

bool deleteFrontierLogin(const std::wstring& path) {
    if (path.empty()) return true;
    if (DeleteFileW(path.c_str())) return true;
    const DWORD code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

}  // namespace edvr::installer
