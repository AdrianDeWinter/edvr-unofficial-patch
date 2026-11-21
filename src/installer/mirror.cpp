#include "mirror.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>

#include "../common/iniedit.h"
#include "../common/mirror_location.h"
#include "state.h"

namespace edvr::installer {
namespace {

const wchar_t* kIni = L"edup.ini";
const wchar_t* kFlatIni = L"edup-flat.ini";
const wchar_t* kBaseIni = L"edup.ini.base";
const wchar_t* kFlatBaseIni = L"edup-flat.ini.base";
const wchar_t* kLegacyIni = L"edvr.ini";
const wchar_t* kLegacyFlatIni = L"edvr-flat.ini";
const wchar_t* kLegacyBaseIni = L"edvr.ini.base";
const wchar_t* kLegacyFlatBaseIni = L"edvr-flat.ini.base";
const wchar_t* kStateIni = L"state.ini";
const wchar_t* kD3d11 = L"d3d11.dll";
const wchar_t* kOpenvr = L"openvr_api.dll";
const wchar_t* kBackupSub = L"backup";

std::wstring localAppDataFolder() {
    PWSTR path = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        base = path;
        CoTaskMemFree(path);
    } else {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) base = env;
    }
    return base;
}

bool ensureDirTree(const std::wstring& path) {
    if (path.empty() || dirExists(path)) return true;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        if (!ensureDirTree(path.substr(0, slash))) return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && dirExists(path);
}

// Every file this writes, either direction, can legitimately be one Windows
// marked read-only (a launcher's file verification does this to files it
// considers its own), and a replace simply fails against that.
void clearReadOnly(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
}

// The largest file copied: the d3d11.dll and openvr_api.dll of the backup pair
// are megabytes, and a file that is not is not one this mirror has any business
// holding.
const size_t kCopyLimit = 256u << 20;

// A file copied whole, and put in place with one replace: what was at `to` is
// either still there or entirely replaced. This was CopyFileW in place, and a
// failure part-way -- a full disk, a scanner taking the handle -- left the
// mirror's only copy of somebody's settings truncated.
bool copyOver(const std::wstring& from, const std::wstring& to) {
    clearReadOnly(to);
    std::string bytes;
    if (!readFileBytes(from, &bytes, kCopyLimit)) return false;
    return writeFileAtomic(to, bytes);
}

// One ini into the mirror as its newest generation (see iniedit.h). A
// `checkpoint` -- an installer run -- keeps the copy it replaces as .1, and .1
// as .2; anything else is a change made a moment ago and replaces the newest in
// place.
bool mirrorIni(const std::wstring& from, const std::wstring& mirrorDir, const wchar_t* name,
               bool checkpoint) {
    std::string bytes;
    if (!readFileBytes(from, &bytes) || bytes.empty()) return false;
    for (int g = 0; g < kMirrorGenerations; ++g) clearReadOnly(generationPath(mirrorDir, name, g));
    return writeGenerations(mirrorDir, name, bytes, checkpoint);
}

// Rotates a pre-install backup into .1 / .2 generations beside a live ini that already lives in
// `mirrorDir`, so checkpoint history is preserved without overwriting the live generation 0.
bool checkpointPreviousIni(const std::wstring& previousPath, const std::wstring& mirrorDir,
                           const wchar_t* name) {
    std::string prevBytes, curBytes;
    if (!readFileBytes(previousPath, &prevBytes) || prevBytes.empty()) return false;
    const std::wstring curPath = joinPath(mirrorDir, name);
    if (readFileBytes(curPath, &curBytes) && curBytes == prevBytes) return false;
    for (int g = 1; g < kMirrorGenerations; ++g) clearReadOnly(generationPath(mirrorDir, name, g));
    for (int g = kMirrorGenerations - 1; g >= 2; --g) {
        const std::wstring src = generationPath(mirrorDir, name, g - 1);
        const std::wstring dst = generationPath(mirrorDir, name, g);
        if (fileExists(src)) MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return writeFileAtomic(generationPath(mirrorDir, name, 1), prevBytes);
}

// Finds the newest non-empty generation of `name` (or its legacy counterpart) in `mirrorDir`,
// falling back to the sibling legacy %LOCALAPPDATA%\EDVR\<install> directory.
std::wstring findMirroredIni(const std::wstring& mirrorDir, const wchar_t* name,
                             const wchar_t* legacyName) {
    std::wstring found = newestGeneration(mirrorDir, name);
    if (!found.empty()) return found;
    if (legacyName) {
        found = newestGeneration(mirrorDir, legacyName);
        if (!found.empty()) return found;
    }
    const std::wstring legacyDir = legacyMirrorDirFor(mirrorDir);
    if (!legacyDir.empty() && _wcsicmp(legacyDir.c_str(), mirrorDir.c_str()) != 0) {
        found = newestGeneration(legacyDir, name);
        if (!found.empty()) return found;
        if (legacyName) {
            found = newestGeneration(legacyDir, legacyName);
            if (!found.empty()) return found;
        }
    }
    return std::wstring();
}

std::wstring findMirroredFile(const std::wstring& mirrorDir, const wchar_t* name,
                              const wchar_t* legacyName = nullptr) {
    const std::wstring primary = joinPath(mirrorDir, name);
    if (fileExists(primary)) return primary;
    if (legacyName) {
        const std::wstring legHere = joinPath(mirrorDir, legacyName);
        if (fileExists(legHere)) return legHere;
    }
    const std::wstring legacyDir = legacyMirrorDirFor(mirrorDir);
    if (!legacyDir.empty() && _wcsicmp(legacyDir.c_str(), mirrorDir.c_str()) != 0) {
        const std::wstring inLeg = joinPath(legacyDir, name);
        if (fileExists(inLeg)) return inLeg;
        if (legacyName) {
            const std::wstring legInLeg = joinPath(legacyDir, legacyName);
            if (fileExists(legInLeg)) return legInLeg;
        }
    }
    return std::wstring();
}

bool copyMissingFile(const std::wstring& src, const std::wstring& dst) {
    if (fileExists(dst) || !fileExists(src)) return false;
    const size_t slash = dst.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !ensureDirTree(dst.substr(0, slash))) return false;
    return copyOver(src, dst);
}

bool copyMissingTree(const std::wstring& srcDir, const std::wstring& dstDir, bool skipLauncherRuntime) {
    if (!dirExists(srcDir)) return false;
    bool copiedAny = false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(srcDir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring src = joinPath(srcDir, name);
        const std::wstring dst = joinPath(dstDir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            copiedAny |= copyMissingTree(src, dst, false);
        } else {
            if (skipLauncherRuntime) {
                std::wstring lower = name;
                for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
                if (lower.compare(0, 15, L"ed-up-launcher.") == 0 ||
                    lower.compare(0, 5, L"plan-") == 0 ||
                    lower.compare(0, 7, L"result-") == 0) {
                    continue;
                }
            }
            copiedAny |= copyMissingFile(src, dst);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return copiedAny;
}

// UTC, formatted the same way state.cpp's utcNow() is -- one clock, one
// format, wherever a timestamp reaches the report.
std::string fileSavedUtc(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return std::string();
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&data.ftLastWriteTime, &utc)) return std::string();
    char buf[32];
    sprintf_s(buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
              utc.wMinute, utc.wSecond);
    return std::string(buf);
}

}  // namespace

std::wstring defaultMirrorRoot() {
    const std::wstring base = localAppDataFolder();
    return base.empty() ? std::wstring() : joinPath(base, L"EDUP");
}

std::wstring legacyMirrorRootFor(const std::wstring& root) {
    std::wstring trimmed = root;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const size_t slash = trimmed.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    const std::wstring leaf = trimmed.substr(slash + 1);
    if (_wcsicmp(leaf.c_str(), L"EDUP") != 0) return std::wstring();
    return trimmed.substr(0, slash + 1) + L"EDVR";
}

std::wstring legacyMirrorDirFor(const std::wstring& mirrorDir) {
    std::wstring trimmed = mirrorDir;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const size_t slash = trimmed.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    const std::wstring parent = trimmed.substr(0, slash);
    const std::wstring leaf = trimmed.substr(slash + 1);
    const std::wstring legacyRoot = legacyMirrorRootFor(parent);
    if (legacyRoot.empty() || leaf.empty()) return std::wstring();
    return joinPath(legacyRoot, leaf);
}

bool migrateLegacyDataRoot(const std::wstring& newRoot) {
    const std::wstring oldRoot = legacyMirrorRootFor(newRoot);
    if (oldRoot.empty() || !dirExists(oldRoot)) return false;
    if (!ensureDirTree(newRoot)) return false;

    bool migratedAny = false;
    migratedAny |= copyMissingFile(joinPath(oldRoot, L"game_vram.txt"), joinPath(newRoot, L"game_vram.txt"));
    migratedAny |= copyMissingFile(joinPath(oldRoot, L"last-station.txt"), joinPath(newRoot, L"last-station.txt"));
    migratedAny |= copyMissingTree(joinPath(oldRoot, L"launcher"), joinPath(newRoot, L"launcher"), true);
    migratedAny |= copyMissingTree(joinPath(oldRoot, L"payload"), joinPath(newRoot, L"payload"), false);

    // Per-install folders: copy seat memory, install state, merge bases and DLL backup pairs.
    // Legacy edvr*.ini files are left in oldRoot so surveyTarget/planInstall can compare timestamps
    // against any newer <gameDir>\edvr*.ini before migrating to edup*.ini.
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(oldRoot, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L".." ||
                _wcsicmp(name.c_str(), L"launcher") == 0 ||
                _wcsicmp(name.c_str(), L"payload") == 0)
                continue;
            const std::wstring srcSub = joinPath(oldRoot, name);
            const std::wstring dstSub = joinPath(newRoot, name);
            migratedAny |= copyMissingFile(joinPath(srcSub, L"seat.txt"), joinPath(dstSub, L"seat.txt"));
            migratedAny |= copyMissingFile(joinPath(srcSub, kStateIni), joinPath(dstSub, kStateIni));
            migratedAny |= copyMissingFile(joinPath(srcSub, kLegacyBaseIni), joinPath(dstSub, kBaseIni));
            migratedAny |= copyMissingFile(joinPath(srcSub, kLegacyFlatBaseIni), joinPath(dstSub, kFlatBaseIni));
            migratedAny |= copyMissingTree(joinPath(srcSub, kBackupSub), joinPath(dstSub, kBackupSub), false);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return migratedAny;
}

std::wstring mirrorDirFor(const GameInstall& game, const std::wstring& root) {
    if (root.empty() || game.dir.empty()) return std::wstring();
    return edvr::mirrorDirForSource(root, game.dir, game.source);
}

MirrorInfo readMirror(const std::wstring& mirrorDir) {
    MirrorInfo info;
    info.dir = mirrorDir;
    if (mirrorDir.empty()) return info;

    const std::wstring iniPath = findMirroredIni(mirrorDir, kIni, kLegacyIni);
    const std::wstring flatIniPath = findMirroredIni(mirrorDir, kFlatIni, kLegacyFlatIni);
    info.hasIni = !iniPath.empty();
    info.hasFlatIni = !flatIniPath.empty();
    info.hasBaseIni = !findMirroredFile(mirrorDir, kBaseIni, kLegacyBaseIni).empty();
    info.hasFlatBaseIni = !findMirroredFile(mirrorDir, kFlatBaseIni, kLegacyFlatBaseIni).empty();
    info.hasState = !findMirroredFile(mirrorDir, kStateIni).empty();

    const std::wstring backupSub = joinPath(mirrorDir, kBackupSub);
    const std::wstring legacyDir = legacyMirrorDirFor(mirrorDir);
    const std::wstring legacyBackupSub = legacyDir.empty() ? std::wstring() : joinPath(legacyDir, kBackupSub);
    info.hasBackupPair =
        fileExists(joinPath(backupSub, kD3d11)) || fileExists(joinPath(backupSub, kOpenvr)) ||
        (!legacyBackupSub.empty() &&
         (fileExists(joinPath(legacyBackupSub, kD3d11)) || fileExists(joinPath(legacyBackupSub, kOpenvr))));

    const std::string iniWhen = info.hasIni ? fileSavedUtc(iniPath) : std::string();
    const std::string flatWhen = info.hasFlatIni ? fileSavedUtc(flatIniPath) : std::string();
    info.savedUtc = flatWhen > iniWhen ? flatWhen : iniWhen;
    return info;
}

std::vector<std::string> settingsToRestore(const std::wstring& targetDir, const MirrorInfo& mirror,
                                           const std::string& scope) {
    std::vector<std::string> names;
    const bool hasVr = fileExists(joinPath(targetDir, kIni)) || fileExists(joinPath(targetDir, kLegacyIni));
    const bool hasFlat =
        fileExists(joinPath(targetDir, kFlatIni)) || fileExists(joinPath(targetDir, kLegacyFlatIni));
    if (scopeHasVrIni(scope) && mirror.hasIni && !hasVr)
        names.push_back("edup.ini");
    if (scopeHasFlatIni(scope) && mirror.hasFlatIni && !hasFlat)
        names.push_back("edup-flat.ini");
    return names;
}

std::string settingsNamesOf(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) out += (out.empty() ? "" : " and ") + name;
    return out;
}

MirrorResult updateMirror(const std::wstring& gameDir, const std::wstring& backupDir,
                          const std::wstring& mirrorDir) {
    MirrorResult result;
    if (mirrorDir.empty() || !ensureDirTree(mirrorDir)) return result;
    result.ok = true;

    const bool sameDir = _wcsicmp(gameDir.c_str(), mirrorDir.c_str()) == 0;
    auto syncOneIni = [&](const wchar_t* name, const wchar_t* legacyName, const char* label) {
        const std::wstring gameIni = joinPath(gameDir, name);
        const std::wstring liveIni = joinPath(mirrorDir, name);
        if (!sameDir && fileExists(gameIni)) {
            if (mirrorIni(gameIni, mirrorDir, name, true)) result.saved.push_back(label);
            return;
        }
        if (fileExists(liveIni)) {
            if (!backupDir.empty()) {
                const std::wstring prevNew = joinPath(backupDir, name);
                const std::wstring prevOld = joinPath(backupDir, legacyName);
                if (fileExists(prevNew)) checkpointPreviousIni(prevNew, mirrorDir, name);
                else if (fileExists(prevOld)) checkpointPreviousIni(prevOld, mirrorDir, name);
            }
            result.saved.push_back(label);
        }
    };
    syncOneIni(kIni, kLegacyIni, "edup.ini");
    syncOneIni(kFlatIni, kLegacyFlatIni, "edup-flat.ini");

    const std::wstring basePath = baseIniPath(gameDir);
    if (fileExists(basePath) && copyOver(basePath, joinPath(mirrorDir, kBaseIni)))
        result.saved.push_back("edup.ini.base");

    const std::wstring flatBasePath = flatBaseIniPath(gameDir);
    if (fileExists(flatBasePath) && copyOver(flatBasePath, joinPath(mirrorDir, kFlatBaseIni)))
        result.saved.push_back("edup-flat.ini.base");

    const std::wstring statePathSrc = statePath(gameDir);
    if (fileExists(statePathSrc) && copyOver(statePathSrc, joinPath(mirrorDir, kStateIni)))
        result.saved.push_back("state.ini");

    if (!backupDir.empty()) {
        const std::wstring d3dSrc = joinPath(backupDir, kD3d11);
        const std::wstring ovrSrc = joinPath(backupDir, kOpenvr);
        const bool haveD3d = fileExists(d3dSrc);
        const bool haveOvr = fileExists(ovrSrc);
        if (haveD3d || haveOvr) {
            const std::wstring backupSub = joinPath(mirrorDir, kBackupSub);
            if (ensureDirTree(backupSub)) {
                bool copiedAny = false;
                if (haveD3d) copiedAny |= copyOver(d3dSrc, joinPath(backupSub, kD3d11));
                if (haveOvr) copiedAny |= copyOver(ovrSrc, joinPath(backupSub, kOpenvr));
                if (copiedAny)
                    result.saved.push_back("the last d3d11.dll/openvr_api.dll backup pair");
            }
        }
    }
    return result;
}

MirrorResult updateMirrorIni(const std::wstring& gameDir, const std::wstring& mirrorDir) {
    MirrorResult result;
    if (mirrorDir.empty() || !ensureDirTree(mirrorDir)) return result;

    if (_wcsicmp(gameDir.c_str(), mirrorDir.c_str()) == 0) {
        result.ok = true;
        return result;
    }

    const std::wstring iniPath = joinPath(gameDir, kIni);
    if (fileExists(iniPath) && mirrorIni(iniPath, mirrorDir, kIni, false)) {
        result.ok = true;
        result.saved.push_back("edup.ini");
    }

    const std::wstring flatIniPath = joinPath(gameDir, kFlatIni);
    if (fileExists(flatIniPath) && mirrorIni(flatIniPath, mirrorDir, kFlatIni, false)) {
        result.ok = true;
        result.saved.push_back("edup-flat.ini");
    }
    return result;
}

bool restoreFromMirror(const std::wstring& gameDir, const MirrorInfo& info, const std::string& scope,
                       std::vector<std::string>* notes, const std::wstring& targetSettingsDir) {
    auto note = [&](const std::string& s) {
        if (notes) notes->push_back(s);
    };

    const std::wstring destSettingsDir = targetSettingsDir.empty() ? gameDir : targetSettingsDir;
    const std::vector<std::string> wanted = settingsToRestore(destSettingsDir, info, scope);
    if (wanted.empty()) return false;
    if (!ensureDirTree(destSettingsDir)) return false;

    bool restoredAll = true;
    bool restoredAny = false;
    struct Settings {
        const wchar_t* leaf;
        const wchar_t* legacyLeaf;
        const char*    name;
        const char*    what;
    };
    const Settings settings[] = {
        {kIni, kLegacyIni, "edup.ini", "edup.ini"},
        {kFlatIni, kLegacyFlatIni, "edup-flat.ini", "edup-flat.ini (the flat profile's own settings)"},
    };
    for (const Settings& one : settings) {
        if (std::find(wanted.begin(), wanted.end(), std::string(one.name)) == wanted.end()) continue;
        const std::wstring source = findMirroredIni(info.dir, one.leaf, one.legacyLeaf);
        const std::wstring dst = joinPath(destSettingsDir, one.leaf);
        if (source.empty() ||
            (_wcsicmp(source.c_str(), dst.c_str()) != 0 && !copyOver(source, dst))) {
            note(std::string("Could not restore ") + one.what +
                 " from the copy kept outside the game folder.");
            restoredAll = false;
            continue;
        }
        restoredAny = true;
        note(std::string("Restored ") + one.what + " from the copy kept outside the game folder.");
    }
    if (!restoredAny) return false;

    if (info.hasBaseIni || info.hasFlatBaseIni || info.hasState) {
        if (ensureDirTree(stateDirPath(gameDir))) {
            bool copiedAny = false;
            const std::wstring baseSrc = findMirroredFile(info.dir, kBaseIni, kLegacyBaseIni);
            if (!baseSrc.empty() && !fileExists(baseIniPath(gameDir)))
                copiedAny |= copyOver(baseSrc, baseIniPath(gameDir));
            const std::wstring flatBaseSrc = findMirroredFile(info.dir, kFlatBaseIni, kLegacyFlatBaseIni);
            if (!flatBaseSrc.empty() && !fileExists(flatBaseIniPath(gameDir)))
                copiedAny |= copyOver(flatBaseSrc, flatBaseIniPath(gameDir));
            const std::wstring stateSrc = findMirroredFile(info.dir, kStateIni);
            if (!stateSrc.empty() && !fileExists(statePath(gameDir)))
                copiedAny |= copyOver(stateSrc, statePath(gameDir));
            if (copiedAny) {
                note("Restored the install record too, so this still merges as an update rather "
                     "than starting over.");
            }
        }
    }

    if (info.hasBackupPair) {
        const std::wstring restoreDir =
            joinPath(backupRootPath(gameDir), L"restored-" + timestampName());
        if (ensureDirTree(restoreDir)) {
            std::wstring backupSub = joinPath(info.dir, kBackupSub);
            if (!fileExists(joinPath(backupSub, kD3d11)) && !fileExists(joinPath(backupSub, kOpenvr))) {
                const std::wstring legacyDir = legacyMirrorDirFor(info.dir);
                if (!legacyDir.empty()) backupSub = joinPath(legacyDir, kBackupSub);
            }
            const std::wstring d3dSrc = joinPath(backupSub, kD3d11);
            const std::wstring ovrSrc = joinPath(backupSub, kOpenvr);
            bool any = false;
            if (fileExists(d3dSrc)) any |= copyOver(d3dSrc, joinPath(restoreDir, kD3d11));
            if (fileExists(ovrSrc)) any |= copyOver(ovrSrc, joinPath(restoreDir, kOpenvr));
            if (any) {
                note("Restored the last backed-up d3d11.dll/openvr_api.dll pair into edup_backup\\" +
                     toUtf8(leafOf(restoreDir)) + "\\.");
            }
        }
    }
    return restoredAll;
}

}  // namespace edvr::installer
