#include "launch_route.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>

#include "../common/elite_graphics_folder.h"
#include "../common/iniedit.h"
#include "../common/stereo_mode.h"
#include "detect.h"
#include "direct_launch.h"
#include "logbundle.h"
#include "mirror.h"
#include "probe.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

std::wstring parentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return std::wstring();
    const std::wstring parent = path.substr(0, slash);
    // "C:" alone is a drive's current folder, not a place to look.
    return parent.size() <= 2 && parent.back() == L':' ? std::wstring() : parent;
}

bool sourceIs(const std::wstring& source, const wchar_t* name) { return _wcsicmp(source.c_str(), name) == 0; }

struct CloseTarget {
    const std::vector<unsigned long>* pids;
    bool posted;
};

BOOL CALLBACK postCloseTo(HWND window, LPARAM param) {
    CloseTarget* target = reinterpret_cast<CloseTarget*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (std::find(target->pids->begin(), target->pids->end(), static_cast<unsigned long>(pid)) != target->pids->end()) {
        PostMessageW(window, WM_CLOSE, 0, 0);
        target->posted = true;
    }
    return TRUE;
}

// Sleeps up to `ms`, in slices, and stops early when `cancel` is set.
void sleepCancellable(unsigned ms, const std::atomic<bool>* cancel) {
    unsigned slept = 0;
    while (slept < ms && !(cancel && cancel->load())) {
        const unsigned slice = std::min(25u, ms - slept);
        Sleep(slice);
        slept += slice;
    }
}

}  // namespace

std::wstring findEdLaunch(const std::wstring& gameDir) {
    std::wstring folder = gameDir;
    for (int level = 0; level < 8 && !folder.empty(); ++level) {
        for (const std::wstring& candidate : {joinPath(folder, L"EDLaunch.exe"), joinPath(joinPath(folder, L"EDLaunch"), L"EDLaunch.exe")}) {
            if (fileExists(candidate)) return candidate;
        }
        folder = parentOf(folder);
    }
    return std::wstring();
}

std::wstring findEdLaunchIn(const std::vector<std::wstring>& roots) {
    for (const std::wstring& root : roots) {
        if (root.empty()) continue;
        for (const std::wstring& candidate : {joinPath(root, L"EDLaunch.exe"), joinPath(joinPath(root, L"EDLaunch"), L"EDLaunch.exe")}) {
            if (fileExists(candidate)) return candidate;
        }
    }
    return std::wstring();
}

LaunchRoute routeFor(const std::wstring& source, const std::wstring& gameDir, bool vr,
                     const std::vector<std::wstring>* frontierRoots, bool direct) {
    LaunchRoute route;
    if (sourceIs(source, L"Steam")) {
        route.kind = RouteKind::Steam;
        route.uri = std::wstring(L"steam://rungameid/") + kSteamAppId;
        return route;
    }
    if (sourceIs(source, L"Epic")) {
        if (direct && findEdLaunch(gameDir).empty()) {
            route.why = "EDLaunch.exe was not found above this Epic install, so it cannot be stood in for.";
            return route;
        }
        // Epic's protocol activation address, the counterpart of steam://rungameid.
        // The three ids are the catalog's, and the colons are encoded so the
        // address survives ShellExecute. With the checkbox on, the mode is carried
        // by the stand-in Epic then starts, not by this address.
        route.kind = RouteKind::Epic;
        route.uri = std::wstring(L"com.epicgames.launcher://apps/") + kEpicNamespaceId + L"%3A" +
                    kEpicCatalogId + L"%3A" + kEpicAppId + L"?action=launch&silent=true";
        route.carriesMode = direct;
        return route;
    }
    std::wstring edlaunch = findEdLaunch(gameDir);
    if (edlaunch.empty() && sourceIs(source, L"Frontier launcher")) {
        // The window asks for the route on every refresh, and this walks the uninstall registry.
        static const std::vector<std::wstring> machineRoots = frontierLauncherRoots();
        edlaunch = findEdLaunchIn(frontierRoots ? *frontierRoots : machineRoots);
    }
    if (edlaunch.empty()) {
        route.why = "EDLaunch.exe was not found for this folder, so there is nothing to start the game with from here.";
        return route;
    }
    route.kind = RouteKind::Frontier;
    route.exe = edlaunch;
    route.workDir = parentOf(edlaunch);
    route.args = std::wstring(L"/autorun /autoquit /edo ") + (vr ? L"/vr" : L"/novr");
    route.carriesMode = true;
    return route;
}

RouteOps realRouteOps() {
    RouteOps ops;
    ops.underWine = runningUnderWine();
    ops.openUri = [](const std::wstring& uri) {
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    };
    ops.spawn = [](const std::wstring& exe, const std::wstring& args, const std::wstring& workDir) {
        std::wstring line = L"\"" + exe + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), &line[0], nullptr, nullptr, FALSE, 0, nullptr,
                            workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
            return false;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    };
    return ops;
}

const char* elevatedRefusal() {
    return "This launcher is running with administrator rights, and Elite must not be started from an elevated launcher. Close it and open it again normally (without Run as administrator), or start Elite from Steam, Epic, or Frontier's launcher.";
}

bool startRoute(const LaunchRoute& route, const RouteOps& ops, bool elevated, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    if (elevated) return fail(elevatedRefusal());
    switch (route.kind) {
        case RouteKind::Steam:
        case RouteKind::Epic: {
            // Proton consumes rungameid for its own SteamGameId as a restart request, even
            // without a restart supervisor (runinprefix). The documented run URI is forwarded.
            // ValveSoftware/Proton, steam_helper/steam.c: steam_command_handler;
            // https://partner.steamgames.com/doc/sdk/api#initialization_and_shutdown
            const std::wstring uri = route.kind == RouteKind::Steam && ops.underWine
                ? std::wstring(L"steam://run/") + kSteamAppId : route.uri;
            if (!ops.openUri || !ops.openUri(uri)) return fail("Windows would not open the store's launch address.");
            return true;
        }
        case RouteKind::Frontier:
            if (!ops.spawn || !ops.spawn(route.exe, route.args, route.workDir)) return fail("EDLaunch.exe could not be started.");
            return true;
        default:
            return fail(route.why.empty() ? "There is nothing to start the game with." : route.why);
    }
}

namespace {

bool hasFlag(const std::wstring& line, const wchar_t* flag) {
    std::wstring hay = line, needle = flag;
    for (wchar_t& c : hay) c = static_cast<wchar_t>(towlower(c));
    for (wchar_t& c : needle) c = static_cast<wchar_t>(towlower(c));
    size_t at = 0;
    while ((at = hay.find(needle, at)) != std::wstring::npos) {
        const bool boundary = at == 0 || hay[at - 1] == L' ';
        const size_t end = at + needle.size();
        if (boundary && (end == hay.size() || hay[end] == L' ')) return true;
        at += needle.size();
    }
    return false;
}

std::wstring argsAfterExe(const std::wstring& commandLine) {
    size_t i = 0;
    while (i < commandLine.size() && (commandLine[i] == L' ' || commandLine[i] == L'\t')) ++i;
    if (i >= commandLine.size()) return std::wstring();
    if (commandLine[i] == L'"') {
        const size_t end = commandLine.find(L'"', i + 1);
        i = end == std::wstring::npos ? commandLine.size() : end + 1;
    } else {
        while (i < commandLine.size() && commandLine[i] != L' ' && commandLine[i] != L'\t') ++i;
    }
    while (i < commandLine.size() && (commandLine[i] == L' ' || commandLine[i] == L'\t')) ++i;
    return commandLine.substr(i);
}

std::wstring readSmallText(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::wstring();
    char buf[80]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(f);
    if (!ok) return std::wstring();
    std::string text(buf, buf + read);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return std::wstring(text.begin(), text.end());
}

bool writeSmallText(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == text.size();
}

std::wstring modulePath() {
    wchar_t buf[1024]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, 1024);
    return (n > 0 && n < 1024) ? std::wstring(buf, n) : std::wstring();
}

// `src` takes `dest`'s name. A failure leaves `dest` where it was when it already existed.
bool replaceWith(const std::wstring& dest, const std::wstring& src) {
    if (!fileExists(dest)) return MoveFileW(src.c_str(), dest.c_str()) != FALSE;
    if (ReplaceFileW(dest.c_str(), src.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) return true;
    const std::wstring aside = dest + L".edvr-aside";
    DeleteFileW(aside.c_str());
    if (!MoveFileW(dest.c_str(), aside.c_str())) return false;
    if (!MoveFileW(src.c_str(), dest.c_str())) {
        MoveFileW(aside.c_str(), dest.c_str());
        return false;
    }
    DeleteFileW(aside.c_str());
    return true;
}

// EDLaunch_orig.exe in the same places findEdLaunch looks, for a live file that is already gone.
std::wstring findEdLaunchOrig(const std::wstring& gameDir) {
    std::wstring folder = gameDir;
    for (int level = 0; level < 8 && !folder.empty(); ++level) {
        for (const std::wstring& candidate : {joinPath(folder, kEdLaunchOrigName), joinPath(joinPath(folder, L"EDLaunch"), kEdLaunchOrigName)}) {
            if (fileExists(candidate)) return candidate;
        }
        folder = parentOf(folder);
    }
    return std::wstring();
}

}  // namespace

std::wstring edLaunchShimCommand(const std::wstring& origExe, const std::wstring& commandLine, bool vr,
                                 bool chooseMode) {
    std::wstring args = argsAfterExe(commandLine);
    auto add = [&](const wchar_t* flag) {
        if (hasFlag(args, flag)) return;
        if (!args.empty()) args += L' ';
        args += flag;
    };
    add(L"/autorun");
    add(L"/autoquit");
    add(L"/edo");
    // /novr contains the letters of /vr, so the mode is chosen only when neither flag is present.
    if (chooseMode && !hasFlag(args, L"/vr") && !hasFlag(args, L"/novr")) add(vr ? L"/vr" : L"/novr");
    return L"\"" + origExe + L"\" " + args;
}

int runEdLaunchShimIfInvoked() {
    const std::wstring self = modulePath();
    const std::wstring leaf = self.substr(self.find_last_of(L"\\/") == std::wstring::npos ? 0 : self.find_last_of(L"\\/") + 1);
    if (_wcsicmp(leaf.c_str(), L"EDLaunch.exe") != 0) return -1;

    const std::wstring dir = parentOf(self);
    const std::wstring mode = readSmallText(joinPath(dir, kEdLaunchModeName));
    std::string error;
    if (!startGameFromEpicCommand(dir, GetCommandLineW(), mode == L"vr", &error)) {
        MessageBoxW(nullptr, widenUtf8(error).c_str(), L"ED Unofficial Patch", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}

bool writeEdLaunchMode(const std::wstring& gameDir, bool vr) {
    const std::wstring edlaunch = findEdLaunch(gameDir);
    if (edlaunch.empty()) return false;
    return writeSmallText(joinPath(parentOf(edlaunch), kEdLaunchModeName), vr ? "vr\n" : "flat\n");
}

bool installEdLaunchShim(const std::wstring& selfExe, const std::wstring& gameDir, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    if (!fileExists(selfExe)) return true;
    const std::wstring edlaunch = findEdLaunch(gameDir);
    if (edlaunch.empty()) return fail("EDLaunch.exe was not found above this Epic install.");
    const std::wstring dir = parentOf(edlaunch);
    const std::wstring orig = joinPath(dir, kEdLaunchOrigName);
    const std::wstring marker = joinPath(dir, kEdLaunchMarkerName);
    const std::string selfHash = sha256File(selfExe);
    const std::string liveHash = sha256File(edlaunch);
    if (selfHash.empty() || liveHash.empty()) return fail("EDLaunch.exe could not be read.");
    const std::wstring recorded = readSmallText(marker);
    const bool marked = !recorded.empty();
    if (marked && liveHash == selfHash) return true;

    // The marker names the stand-in we wrote. The same hash means that copy is still there
    // and this launcher has been rebuilt, so only the copy is replaced. A different hash is
    // a game update putting Frontier's binary back, and that binary becomes the saved one.
    const bool replaceCopyOnly = marked && narrowUtf8(recorded) == liveHash;
    if (!replaceCopyOnly && !marked && fileExists(orig))
        return fail("EDLaunch_orig.exe is already beside EDLaunch.exe, so the stand-in was not installed.");

    const std::wstring staged = joinPath(dir, L"EDLaunch.exe.edvr-new");
    const std::wstring liveAside = joinPath(dir, L"EDLaunch.exe.edvr-old");
    const std::wstring savedAside = joinPath(dir, L"EDLaunch_orig.exe.edvr-bak");
    auto drop = [](const std::wstring& path) { DeleteFileW(path.c_str()); };
    const std::string oldMarker = narrowUtf8(recorded);
    auto restoreMarker = [&]() {
        if (marked) writeSmallText(marker, oldMarker);
        else drop(marker);
    };
    drop(staged);
    if (!CopyFileW(selfExe.c_str(), staged.c_str(), FALSE)) {
        drop(staged);
        return fail("The stand-in could not be copied into EDLaunch.exe's place.");
    }

    if (replaceCopyOnly) {
        drop(liveAside);
        if (!MoveFileW(edlaunch.c_str(), liveAside.c_str())) {
            drop(staged);
            return fail("EDLaunch.exe is in use, so the stand-in could not take its place.");
        }
        if (!MoveFileW(staged.c_str(), edlaunch.c_str())) {
            MoveFileW(liveAside.c_str(), edlaunch.c_str());
            drop(staged);
            return fail("The stand-in could not be copied into EDLaunch.exe's place.");
        }
        const std::string hash = sha256File(edlaunch);
        if (hash.empty() || !writeSmallText(marker, hash)) {
            if (replaceWith(edlaunch, liveAside)) restoreMarker();
            else if (!hash.empty()) writeSmallText(marker, hash);
            return fail("The stand-in was copied, but its marker could not be written.");
        }
        drop(liveAside);
        return true;
    }

    const bool hadSaved = fileExists(orig);
    if (hadSaved) {
        drop(savedAside);
        if (!MoveFileW(orig.c_str(), savedAside.c_str())) {
            drop(staged);
            return fail("The saved EDLaunch is in use, so it could not be replaced.");
        }
    }
    if (!MoveFileW(edlaunch.c_str(), orig.c_str())) {
        if (hadSaved) MoveFileW(savedAside.c_str(), orig.c_str());
        drop(staged);
        return fail("EDLaunch.exe is in use, so the stand-in could not take its place.");
    }
    if (!MoveFileW(staged.c_str(), edlaunch.c_str())) {
        // The live file is at orig. Put it back before restoring the previous saved binary over that name.
        if (MoveFileW(orig.c_str(), edlaunch.c_str()) && hadSaved) MoveFileW(savedAside.c_str(), orig.c_str());
        drop(staged);
        return fail("The stand-in could not be copied into EDLaunch.exe's place.");
    }
    const std::string hash = sha256File(edlaunch);
    if (hash.empty() || !writeSmallText(marker, hash)) {
        if (replaceWith(edlaunch, orig)) {
            if (hadSaved) MoveFileW(savedAside.c_str(), orig.c_str());
            restoreMarker();
        } else if (!hash.empty()) {
            writeSmallText(marker, hash);
        }
        return fail("The stand-in was copied, but its marker could not be written.");
    }
    if (hadSaved) drop(savedAside);
    return true;
}

bool removeEdLaunchShim(const std::wstring& gameDir, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    std::wstring edlaunch = findEdLaunch(gameDir);
    std::wstring orig;
    std::wstring dir;
    if (!edlaunch.empty()) {
        dir = parentOf(edlaunch);
        orig = joinPath(dir, kEdLaunchOrigName);
        if (!fileExists(joinPath(dir, kEdLaunchMarkerName))) return true;
        if (!fileExists(orig)) return fail("The saved EDLaunch is missing, so the original file was left where it is.");
    } else {
        orig = findEdLaunchOrig(gameDir);
        if (orig.empty()) return true;
        dir = parentOf(orig);
        edlaunch = joinPath(dir, L"EDLaunch.exe");
    }
    // Do not delete the live file first: a failed replace has to leave it where it was.
    if (!replaceWith(edlaunch, orig))
        return fail(fileExists(edlaunch) ? "EDLaunch.exe is in use, so the original could not be put back."
                                         : "The original EDLaunch could not be moved back into place.");
    DeleteFileW(joinPath(dir, kEdLaunchMarkerName).c_str());
    DeleteFileW(joinPath(dir, kEdLaunchModeName).c_str());
    return true;
}

bool pidsNamed(const std::wstring& exeName, std::vector<unsigned long>* pids) {
    pids->clear();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool ok = true;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName.c_str()) == 0) pids->push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    } else if (GetLastError() != ERROR_NO_MORE_FILES) {
        ok = false;
    }
    CloseHandle(snapshot);
    return ok;
}

// The one derivation lives with the settings folder's (mirror.h), so Settings.xml and the settings files agree on the prefix.
std::wstring protonSteamLocalAppDataFor(const std::wstring& gameDir) { return protonLocalAppDataForGame(gameDir); }

std::wstring defaultEliteSettingsXmlPath(const std::wstring& gameDir) {
    // Under Wine/Proton, a launcher started outside Elite's own compatdata prefix would otherwise
    // resolve %LOCALAPPDATA% to its own Wine prefix instead of Elite's Proton prefix.
    if (runningUnderWine()) {
        const std::wstring pfxLocal = protonSteamLocalAppDataFor(gameDir);
        if (!pfxLocal.empty()) {
            const std::wstring folder = edvr::eliteGraphicsFolderUnder(pfxLocal);
            if (!folder.empty()) return joinPath(folder, L"Settings.xml");
        }
    }
    PWSTR path = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        base = path;
        CoTaskMemFree(path);
    } else {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) base = env;
    }
    const std::wstring folder = edvr::eliteGraphicsFolderUnder(base);
    return folder.empty() ? std::wstring() : joinPath(folder, L"Settings.xml");
}

edvr::VrHeadsetMode detectVrHeadsetModeFromSettingsXml(const std::wstring& settingsXmlPath) {
    if (settingsXmlPath.empty() || !fileExists(settingsXmlPath)) return edvr::VrHeadsetMode::NotSet;
    std::string xml;
    if (!readFileBytes(settingsXmlPath, &xml, 4ull << 20, nullptr)) return edvr::VrHeadsetMode::NotSet;
    int existing = 0;
    if (!edvr::readStereoscopicMode(xml, &existing) || !edvr::isVrStereoscopicMode(existing))
        return edvr::VrHeadsetMode::NotSet;
    return edvr::vrHeadsetModeFromInt(existing);
}

bool restoreSettingsXmlFromSnapshot(const SettingsXmlSnapshot& snapshot, std::string* error) {
    if (!snapshot.modified || snapshot.path.empty() || snapshot.backupPath.empty() ||
        !fileExists(snapshot.backupPath)) {
        return true;
    }
    std::string bytes;
    if (!readFileBytes(snapshot.backupPath, &bytes, 4ull << 20, error)) return false;
    const DWORD attrs = GetFileAttributesW(snapshot.path.c_str());
    const bool currentlyReadOnly =
        attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0;
    if (currentlyReadOnly) SetFileAttributesW(snapshot.path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    const bool ok = writeFileAtomic(snapshot.path, bytes);
    if (snapshot.wasReadOnly) {
        const DWORD restoredAttrs = GetFileAttributesW(snapshot.path.c_str());
        if (restoredAttrs != INVALID_FILE_ATTRIBUTES)
            SetFileAttributesW(snapshot.path.c_str(), restoredAttrs | FILE_ATTRIBUTE_READONLY);
    }
    if (!ok && error) *error = "Could not restore Elite's Settings.xml at " + narrowUtf8(snapshot.path) + ".";
    return ok;
}

bool prepareSettingsXmlForLaunch(const std::wstring& settingsXmlPath, const std::wstring& backupDir, bool vr,
                                 edvr::VrHeadsetMode preferredVrMode, std::string* error,
                                 const std::wstring& gameExeName, SettingsXmlSnapshot* snapshot) {
    if (snapshot) *snapshot = SettingsXmlSnapshot();
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    // Intentionally require an explicit headset mode when vr_3d_mode is NotSet: for a new VR user
    // who has never chosen one in Settings.xml, we cannot assume the right audio/cinema mode.
    // Existing VR users whose Settings.xml already holds 3..6 are seeded into launcher.ini before
    // this check so their detected choice survives across sessions.
    if (vr && preferredVrMode == edvr::VrHeadsetMode::NotSet) {
        return fail("No VR 3D mode is set. Choose your preferred VR 3D mode in the launcher before launching in VR, or use Play flat.");
    }
    if (!gameExeName.empty()) {
        std::vector<unsigned long> running;
        if (!pidsNamed(gameExeName, &running))
            return fail("Windows would not list the running programs, so Settings.xml was not changed.");
        if (!running.empty())
            return fail("Elite is already running, so Settings.xml was not changed.");
    }
    if (settingsXmlPath.empty() || !fileExists(settingsXmlPath)) {
        if (vr) {
            return fail("Elite's Settings.xml was not found. Launch Elite once to create Settings.xml, or use Play flat.");
        }
        // A brand-new install with no Settings.xml yet starts flat by default in Elite.
        return true;
    }
    std::string xml;
    std::string readErr;
    if (!readFileBytes(settingsXmlPath, &xml, 4ull << 20, &readErr))
        return fail("Elite's Settings.xml could not be read at " + narrowUtf8(settingsXmlPath) +
                    ", so nothing was changed.");

    std::string updated;
    const edvr::StereoWriteStatus status =
        edvr::rewriteStereoscopicMode(xml, vr, preferredVrMode, &updated);
    switch (status) {
        case edvr::StereoWriteStatus::Unchanged:
            return true;
        case edvr::StereoWriteStatus::TagAbsent:
            return fail("Elite's Settings.xml at " + narrowUtf8(settingsXmlPath) +
                        " could not be updated because <StereoscopicMode> and </GraphicsOptions> were not found.");
        case edvr::StereoWriteStatus::NoHeadsetMode:
            return fail("No VR 3D mode is set. Choose your preferred VR 3D mode in the launcher before launching in VR, or use Play flat.");
        case edvr::StereoWriteStatus::ParseFailed:
            return fail("Elite's Settings.xml at " + narrowUtf8(settingsXmlPath) +
                        " could not be parsed, so nothing was changed.");
        case edvr::StereoWriteStatus::Updated:
            break;
    }

    if (backupDir.empty() || !createDirectories(backupDir))
        return fail("Could not create the launcher backup folder for Settings.xml; nothing was changed.");
    // A read-only Settings.xml is a decision the player made (to stop Elite resetting their graphics): it is never
    // unlocked behind their back, the rule writeFileAtomic keeps for every file (iniedit.h). Nothing is backed up or
    // changed; the message says what to do.
    {
        const DWORD attrs = GetFileAttributesW(settingsXmlPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0)
            return fail("Elite's Settings.xml is read-only, so its 3D mode was not changed. Clear read-only on " +
                        narrowUtf8(settingsXmlPath) + ", or set the 3D mode in Elite's own options.");
    }
    const std::wstring originalBackup = joinPath(backupDir, L"Settings.xml.ed-up-original");
    if (!fileExists(originalBackup)) {
        if (!CopyFileW(settingsXmlPath.c_str(), originalBackup.c_str(), TRUE) && !fileExists(originalBackup))
            return fail("Could not save the original Settings.xml backup before changing it; nothing was changed.");
    }
    const std::wstring backupPath = joinPath(backupDir, L"Settings.xml.bak");
    if (fileExists(backupPath)) SetFileAttributesW(backupPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(settingsXmlPath.c_str(), backupPath.c_str(), FALSE))
        return fail("Could not back up Settings.xml before changing it; nothing was changed.");

    const bool wasReadOnly = false;   // refused above
    const bool wrote = writeFileAtomic(settingsXmlPath, updated);
    if (!wrote) {
        return fail("Could not write Elite's Settings.xml at " + narrowUtf8(settingsXmlPath) +
                    ". Check that the file is writable.");
    }
    if (snapshot) {
        snapshot->modified = true;
        snapshot->wasReadOnly = wasReadOnly;
        snapshot->path = settingsXmlPath;
        snapshot->backupPath = backupPath;
    }
    return true;
}

WatchOutcome watchLaunch(const WatchNames& names, const WatchTiming& timing, const std::atomic<bool>* cancel,
                         const std::function<void(WatchEvent)>& notify) {
    auto say = [&](WatchEvent e) {
        if (notify) notify(e);
    };
    auto cancelled = [&]() { return cancel && cancel->load(); };
    WatchOutcome outcome;

    bool sawGame = false, sawEdlaunch = false, announcedWaiting = false;
    unsigned waited = 0, unreadable = 0, polls = 0;
    std::vector<unsigned long> gamePids, edPids;

    // Until the game is seen.
    while (!sawGame) {
        if (cancelled()) {
            outcome.kind = WatchOutcome::Kind::Cancelled;
            return outcome;
        }
        const bool readGame = pidsNamed(names.game, &gamePids);
        const bool readEd = pidsNamed(names.edlaunch, &edPids);
        ++polls;
        if (!readGame || !readEd) {
            ++unreadable;
        } else if (!gamePids.empty()) {
            sawGame = true;
            say(WatchEvent::GameSeen);
            break;
        } else if (!edPids.empty()) {
            sawEdlaunch = true;
            if (!announcedWaiting) {
                announcedWaiting = true;
                say(WatchEvent::WaitingForGame);
            }
        } else if (sawEdlaunch) {
            outcome.kind = WatchOutcome::Kind::EdlaunchLeft;  // gone with the game never started
            return outcome;
        } else if (waited >= timing.startupWaitMs) {
            outcome.kind = WatchOutcome::Kind::NeverStarted;
            return outcome;
        }
        // A list that could not be read the whole time says nothing about the game.
        if (unreadable == polls && waited >= timing.startupWaitMs) {
            outcome.kind = WatchOutcome::Kind::CouldNotWatch;
            return outcome;
        }
        sleepCancellable(timing.pollMs, cancel);
        waited += timing.pollMs;
    }

    // The game is running. Wait for it to end; a poll that cannot read the list is not an exit.
    for (;;) {
        sleepCancellable(timing.pollMs, cancel);
        if (cancelled()) {
            outcome.kind = WatchOutcome::Kind::Cancelled;
            return outcome;
        }
        if (pidsNamed(names.game, &gamePids) && gamePids.empty()) break;
    }
    say(WatchEvent::GameExited);
    outcome.kind = WatchOutcome::Kind::GameRan;

    // Ask EDLaunch's windows to close, then give it a while to go. Whatever it does, it is left to.
    if (pidsNamed(names.edlaunch, &edPids) && !edPids.empty()) {
        CloseTarget target{&edPids, false};
        EnumWindows(postCloseTo, reinterpret_cast<LPARAM>(&target));
        outcome.askedEdlaunchToClose = target.posted;
        unsigned given = 0;
        while (given < timing.closeWaitMs && !cancelled()) {
            if (pidsNamed(names.edlaunch, &edPids) && edPids.empty()) {
                outcome.edlaunchClosed = true;
                break;
            }
            sleepCancellable(std::min(timing.pollMs, 100u), cancel);
            given += std::min(timing.pollMs, 100u);
        }
    } else {
        outcome.edlaunchClosed = true;  // nothing was left to close
    }
    return outcome;
}

}  // namespace edvr::installer
