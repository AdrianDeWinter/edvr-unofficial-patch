// Ported from MachineId.fs of min-ed-launcher, commit 139e4ff7ab9027925648fb27767cb87acdcf290a.
// MIT licence, "Copyright (c) 2017 Chris": see third_party\min_ed_launcher\LICENSE.
#include "machine_id.h"

#include <objbase.h>
#include <bcrypt.h>

#include <cstdio>
#include <vector>

#include "update_util.h"

namespace edvr::installer {
namespace {

bool isTrimmed(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string trimmed(const std::string& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && isTrimmed(text[begin])) ++begin;
    while (end > begin && isTrimmed(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

bool sha1Of(const std::string& bytes, std::vector<unsigned char>* digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD length = 0, got = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &got, 0) >= 0 &&
              length == 20;
    if (ok) {
        digest->assign(length, 0);
        ok = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                        static_cast<ULONG>(bytes.size()), digest->data(), length) >= 0;
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

// The string value, or false when the key or value is missing or is not text.
bool readString(HKEY root, const std::wstring& path, const std::wstring& name, std::wstring* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
    wchar_t buffer[260]{};
    DWORD bytes = sizeof(buffer) - sizeof(wchar_t), type = 0;
    const LONG status = RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    out->assign(buffer, bytes / sizeof(wchar_t));
    while (!out->empty() && out->back() == L'\0') out->pop_back();
    return true;
}

// A GUID as .NET's Guid.ToString() writes it: lower case, hyphens, no braces.
std::wstring newGuidText() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return std::wstring();
    wchar_t text[64]{};
    swprintf_s(text, L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0],
               guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

}  // namespace

std::string makeMachineId(const std::string& machineGuid, const std::string& frontierGuid) {
    std::vector<unsigned char> digest;
    if (!sha1Of(trimmed(machineGuid) + trimmed(frontierGuid), &digest)) return std::string();
    static const char hex[] = "0123456789abcdef";
    std::string id;
    for (size_t i = 0; i < 8; ++i) {  // 8 bytes: the first 16 hex characters
        id.push_back(hex[digest[i] >> 4]);
        id.push_back(hex[digest[i] & 15]);
    }
    return id;
}

bool readMachineId(const MachineIdKeys& keys, std::string* id, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    std::wstring frontier;
    if (!readString(keys.frontierRoot, keys.frontierPath, keys.valueName, &frontier)) {
        const std::wstring made = newGuidText();
        HKEY key = nullptr;
        if (made.empty() ||
            RegCreateKeyExW(keys.frontierRoot, keys.frontierPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return fail("Frontier's machine identity could not be created in the registry");
        const LONG status = RegSetValueExW(key, keys.valueName.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(made.c_str()),
                                           static_cast<DWORD>((made.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        if (status != ERROR_SUCCESS) return fail("Frontier's machine identity could not be created in the registry");
        frontier = made;
    }
    std::wstring machine;
    if (!readString(keys.machineRoot, keys.machinePath, keys.valueName, &machine))
        return fail("Windows' machine identity could not be read from the registry");
    const std::string result = makeMachineId(narrowUtf8(machine), narrowUtf8(frontier));
    if (result.empty()) return fail("the machine identity could not be hashed");
    *id = result;
    return true;
}

}  // namespace edvr::installer
