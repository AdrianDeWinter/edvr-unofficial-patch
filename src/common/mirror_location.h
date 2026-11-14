#pragma once

#include <windows.h>

#include <cwctype>
#include <string>

namespace edvr {

// Only installer-authored source names belong in the install record.
inline std::wstring canonicalMirrorSource(const std::wstring& source) {
    const wchar_t* names[] = {L"Steam", L"Epic", L"Frontier launcher", L"Folder you chose"};
    for (const wchar_t* name : names)
        if (_wcsicmp(source.c_str(), name) == 0) return name;
    return std::wstring();
}

inline std::wstring mirrorFolderName(const std::wstring& gameDir, const std::wstring& source) {
    const size_t slash = gameDir.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? gameDir : gameDir.substr(slash + 1);
    std::wstring slug;
    for (wchar_t c : source) {
        if (iswalnum(c)) slug += static_cast<wchar_t>(towlower(c));
        else if (!slug.empty() && slug.back() != L'-') slug += L'-';
    }
    while (!slug.empty() && slug.back() == L'-') slug.pop_back();
    if (!slug.empty()) name += L"-" + slug;
    return name;
}

inline bool mirrorDirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The menu and seat memory use the store the installer recorded, even when
// several stores share the same product name. Older records retain the old
// path-hint / unique-existing-folder fallback. A lookup never creates files.
inline std::wstring runtimeMirrorDirFor(const std::wstring& localAppData,
                                       const std::wstring& gameDir, bool create) {
    if (localAppData.empty() || gameDir.empty()) return std::wstring();
    const std::wstring root = localAppData + L"\\EDVR";
    const std::wstring record = gameDir + L"\\edvr_install\\state.ini";
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"edvr", L"store_source", L"", value, 64, record.c_str());
    const std::wstring recordedSource = canonicalMirrorSource(value);

    auto exactDir = [&](const std::wstring& source) {
        const std::wstring path = root + L"\\" + mirrorFolderName(gameDir, source);
        if (mirrorDirectoryExists(path)) return path;
        if (!create) return std::wstring();
        if (!mirrorDirectoryExists(root)) CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW(path.c_str(), nullptr);
        return mirrorDirectoryExists(path) ? path : std::wstring();
    };
    if (!recordedSource.empty()) return exactDir(recordedSource);

    std::wstring lowered = gameDir;
    for (wchar_t& c : lowered) c = static_cast<wchar_t>(towlower(c));
    std::wstring inferredSource;
    if (lowered.find(L"\\steamapps\\") != std::wstring::npos) inferredSource = L"Steam";
    else if (lowered.find(L"\\epic games\\") != std::wstring::npos) inferredSource = L"Epic";
    else if (lowered.find(L"frontier_developments\\products") != std::wstring::npos)
        inferredSource = L"Frontier launcher";
    if (!inferredSource.empty()) {
        const std::wstring path = root + L"\\" + mirrorFolderName(gameDir, inferredSource);
        if (mirrorDirectoryExists(path)) return path;
    }
    const std::wstring pattern = root + L"\\" + mirrorFolderName(gameDir, L"") + L"-*";
    WIN32_FIND_DATAW entry{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &entry);
    std::wstring found;
    unsigned count = 0;
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            found = root + L"\\" + entry.cFileName;
            ++count;
        } while (FindNextFileW(handle, &entry));
        FindClose(handle);
    }
    if (count == 1) return found;
    if (!create) return std::wstring();
    if (inferredSource.empty()) inferredSource = L"Folder you chose";
    return exactDir(inferredSource);
}

}  // namespace edvr
