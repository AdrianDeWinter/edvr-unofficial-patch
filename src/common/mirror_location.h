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

inline std::wstring recordedMirrorSource(const std::wstring& record) {
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"edvr", L"store_source", L"", value, 64, record.c_str());
    return canonicalMirrorSource(value);
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

inline std::wstring uniqueExistingMirrorDir(const std::wstring& root, const std::wstring& gameDir) {
    const std::wstring pattern = root + L"\\" + mirrorFolderName(gameDir, L"") + L"-*";
    WIN32_FIND_DATAW entry{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &entry);
    if (handle == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring found;
    unsigned count = 0;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        found = root + L"\\" + entry.cFileName;
        ++count;
    } while (FindNextFileW(handle, &entry));
    const bool complete = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(handle);
    return complete && count == 1 ? found : std::wstring();
}

// Store discovery can relabel an older generic-folder install without copying
// any files. Keep using its mirror in place, including generations and seat
// memory, when its recorded owner matches. An unbound legacy mirror must be
// the sole candidate. An existing store mirror owns its name; another store's
// folder is never adopted, including through an unlabelled generic selection.
// This selection is shared by the installer and runtime and writes nothing.
inline std::wstring mirrorDirForSource(const std::wstring& root, const std::wstring& gameDir,
                                      const std::wstring& source) {
    const std::wstring exact = root + L"\\" + mirrorFolderName(gameDir, source);
    const std::wstring requested = canonicalMirrorSource(source);
    const bool generic = requested == L"Folder you chose";
    if (requested.empty() || (!generic && mirrorDirectoryExists(exact))) return exact;
    const std::wstring legacy = root + L"\\" + mirrorFolderName(gameDir, L"Folder you chose");
    if (!mirrorDirectoryExists(legacy)) return exact;
    const std::wstring owner = recordedMirrorSource(legacy + L"\\state.ini");
    if (!owner.empty() && owner != L"Folder you chose") {
        if (owner == requested) return legacy;
        return generic ? std::wstring() : exact;
    }
    const std::wstring unique = uniqueExistingMirrorDir(root, gameDir);
    if (!unique.empty() && _wcsicmp(unique.c_str(), legacy.c_str()) == 0) return unique;
    return generic ? std::wstring() : exact;
}

// The menu and seat memory use the store the installer recorded, even when
// several stores share the same product name. Older records retain the old
// path-hint / unique-existing-folder fallback. A read-only lookup creates nothing.
inline std::wstring runtimeMirrorDirFor(const std::wstring& localAppData,
                                       const std::wstring& gameDir, bool create) {
    if (localAppData.empty() || gameDir.empty()) return std::wstring();
    const std::wstring root = localAppData + L"\\EDVR";
    const std::wstring record = gameDir + L"\\edvr_install\\state.ini";
    const std::wstring recordedSource = recordedMirrorSource(record);

    auto exactDir = [&](const std::wstring& source) {
        const std::wstring path = mirrorDirForSource(root, gameDir, source);
        if (path.empty()) return std::wstring();
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
    if (!inferredSource.empty()) return exactDir(inferredSource);
    const std::wstring found = uniqueExistingMirrorDir(root, gameDir);
    if (!found.empty()) return found;
    if (!create) return std::wstring();
    return exactDir(L"Folder you chose");
}

}  // namespace edvr
