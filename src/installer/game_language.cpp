#include "game_language.h"

#include <windows.h>

#include <fstream>
#include <vector>

namespace edvr::installer {
namespace {

struct Known {
    const char* code;
    const wchar_t* name;
    const char* argument;  // GameLanguage, two backslash characters
};

const Known kKnown[] = {
    {"en", L"English", "English\\\\UK"},
    {"de", L"Deutsch", "German\\\\DE"},
    {"es", L"Espa\u00f1ol", "Spanish\\\\ES"},
    {"fr", L"Fran\u00e7ais", "French\\\\FR"},
    {"pt-BR", L"Portugu\u00eas", "Portuguese\\\\BR"},
    {"ru", L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439", "Russian\\\\RU"},
};

const Known* knownOf(const std::string& code) {
    for (const Known& row : kKnown) {
        if (_stricmp(row.code, code.c_str()) == 0) return &row;
    }
    return nullptr;
}

bool folderExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

std::string readAll(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return {};
    std::string data;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0 || n > 8 * 1024 * 1024) return {};
    data.resize(static_cast<size_t>(n));
    in.seekg(0);
    in.read(&data[0], n);
    return data;
}

}  // namespace

std::string canonicalGameLanguage(const std::string& code) {
    const Known* row = knownOf(code);
    return row ? row->code : code;
}

std::string systemGameLanguageCode() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) == 0) return "en";
    if (_wcsnicmp(name, L"pt", 2) == 0) return "pt-BR";
    if (_wcsnicmp(name, L"de", 2) == 0) return "de";
    if (_wcsnicmp(name, L"es", 2) == 0) return "es";
    if (_wcsnicmp(name, L"fr", 2) == 0) return "fr";
    if (_wcsnicmp(name, L"ru", 2) == 0) return "ru";
    return "en";
}

const wchar_t* gameLanguageName(const std::string& code) {
    const Known* row = knownOf(code);
    return row ? row->name : L"English";
}

std::string gameLanguageInBytes(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    auto letter = [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    for (size_t i = 0; i + 6 < size; ++i) {
        if (!letter(bytes[i])) continue;
        size_t n = 0;
        while (i + n < size && n < 20 && letter(bytes[i + n])) ++n;
        if (n < 3 || i + n + 3 >= size) continue;
        if (bytes[i + n] == '\\' && bytes[i + n + 1] == '\\' &&
            bytes[i + n + 2] >= 'A' && bytes[i + n + 2] <= 'Z' &&
            bytes[i + n + 3] >= 'A' && bytes[i + n + 3] <= 'Z') {
            return std::string(reinterpret_cast<const char*>(bytes + i), n + 4);
        }
    }
    return {};
}

std::string gameLanguageArgument(const std::wstring& edLaunchDir, const std::string& code) {
    std::string chosen = canonicalGameLanguage(code);
    if (!knownOf(chosen)) chosen = systemGameLanguageCode();
    // min-ed uses a non-English code only when its satellite folder is beside EDLaunch.
    if (chosen != "en" && !edLaunchDir.empty() && !folderExists(join(edLaunchDir, std::wstring(chosen.begin(), chosen.end()))))
        chosen = systemGameLanguageCode();
    if (!edLaunchDir.empty()) {
        const std::wstring dll = chosen == "en"
            ? join(edLaunchDir, L"LocalResources.dll")
            : join(join(edLaunchDir, std::wstring(chosen.begin(), chosen.end())), L"LocalResources.resources.dll");
        const std::string image = readAll(dll);
        const std::string found = gameLanguageInBytes(image.data(), image.size());
        if (!found.empty()) return found;
    }
    const Known* row = knownOf(chosen);
    return row ? row->argument : kKnown[0].argument;
}

}  // namespace edvr::installer
