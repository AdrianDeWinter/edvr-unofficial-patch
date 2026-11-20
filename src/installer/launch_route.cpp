#include "launch_route.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>

#include "detect.h"
#include "logbundle.h"

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
                     const std::vector<std::wstring>* frontierRoots) {
    LaunchRoute route;
    if (sourceIs(source, L"Steam")) {
        route.kind = RouteKind::Steam;
        route.uri = std::wstring(L"steam://rungameid/") + kSteamAppId;
        return route;
    }
    if (sourceIs(source, L"Epic")) {
        route.kind = RouteKind::Epic;
        route.uri = std::wstring(L"com.epicgames.launcher://apps/") + kEpicAppId + L"?action=launch&silent=true";
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
    return "This launcher is running with administrator rights, and Elite must not be started from one. Close it and open it again normally.";
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
