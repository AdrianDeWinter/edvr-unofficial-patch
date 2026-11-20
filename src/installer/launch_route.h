// Starting the game through EDLaunch, and watching until it is over.
//
// This is the route every store can use (docs\design-launcher-2026-11-07.md, "The EDLaunch
// route"):
//
//   Steam      steam://rungameid/359320 (steam://run/359320 under Wine)
//   Epic       com.epicgames.launcher://apps/<app>?action=launch&silent=true
//   Frontier   EDLaunch.exe /autorun /autoquit /edo /vr   (or /novr)
//
// EDLaunch is left running by /autoquit often enough that the launcher watches for the game,
// and when the game has exited it asks EDLaunch's windows to close with WM_CLOSE. It never
// terminates a process. If the game never starts, EDLaunch is probably patching it, and is
// left alone.
//
// Nothing here starts anything by itself: a route is data, startRoute does the starting
// through RouteOps, and tools\launcher_test supplies ops that record instead of act, so the
// strings can be checked without a launcher ever being started.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace edvr::installer {

enum class RouteKind { None, Steam, Epic, Frontier };

struct LaunchRoute {
    RouteKind kind = RouteKind::None;
    std::wstring uri;      // Steam and Epic
    std::wstring exe;      // Frontier: the EDLaunch.exe
    std::wstring args;     // Frontier: its flags, which carry VR or flat
    std::wstring workDir;  // Frontier: where EDLaunch.exe is
    // Only the Frontier route can say VR or flat: Steam's and Epic's start Elite in whatever
    // mode its own settings name.
    bool carriesMode = false;
    std::string why;       // when kind is None: why there is no Play button
};

inline constexpr const wchar_t* kSteamAppId = L"359320";
inline constexpr const wchar_t* kEpicAppId = L"9c203b6ed35846e8a4a9ff1e314f6593";

// The EDLaunch.exe for a game folder: the first one found in a folder above it, or in an
// EDLaunch folder beside one of those (Steam keeps it at the top of the install; Frontier's own
// launcher keeps it in EDLaunch\ beside Products\). Empty when there is none within eight levels.
std::wstring findEdLaunch(const std::wstring& gameDir);

// The EDLaunch.exe in one of these folders, or in an EDLaunch folder inside one. Empty when none has it.
std::wstring findEdLaunchIn(const std::vector<std::wstring>& roots);

// `source` is GameInstall.source: "Steam", "Epic", "Frontier launcher" or "Folder you chose".
// The last two start through EDLaunch where one is found above the folder. Frontier's store keeps
// the game under AppData and EDLaunch under Program Files, so "Frontier launcher" also looks in
// `frontierRoots`, by default frontierLauncherRoots(). A folder the user picked does not: the
// store's EDLaunch would start the store's own copy of the game, not that folder's. With no
// EDLaunch found there is no route.
LaunchRoute routeFor(const std::wstring& source, const std::wstring& gameDir, bool vr,
                     const std::vector<std::wstring>* frontierRoots = nullptr);

struct RouteOps {
    std::function<bool(const std::wstring& uri)> openUri;
    std::function<bool(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir)> spawn;
    bool underWine = false;  // Steam's URI must reach the native client, not Proton's restart event.
};
RouteOps realRouteOps();

// The sentence for a start refused because the launcher is elevated. Direct launch says the same.
const char* elevatedRefusal();

// Starts the route. Refuses when `elevated` is true: the game must never start from an
// elevated process. `error` says why on false.
bool startRoute(const LaunchRoute& route, const RouteOps& ops, bool elevated, std::string* error);

// The names the watcher looks for, so that a rig can use programs of its own.
struct WatchNames {
    std::wstring game = L"EliteDangerous64.exe";
    std::wstring edlaunch = L"EDLaunch.exe";
};

struct WatchTiming {
    unsigned pollMs = 1000;
    unsigned startupWaitMs = 120000;  // neither EDLaunch nor the game shows up in this long
    unsigned closeWaitMs = 10000;     // after WM_CLOSE, how long EDLaunch gets to leave
};

enum class WatchEvent {
    WaitingForGame,   // EDLaunch is up and the game is not
    GameSeen,         // the game is running: the window can step aside
    GameExited,       // the game has ended; EDLaunch is about to be asked to close
};

struct WatchOutcome {
    enum class Kind {
        GameRan,         // the game started and exited
        EdlaunchLeft,    // EDLaunch exited and the game never started
        NeverStarted,    // neither showed up within startupWaitMs
        Cancelled,       // the caller stopped watching; everything was left running
        CouldNotWatch,   // the process list could not be read
    };
    Kind kind = Kind::NeverStarted;
    bool askedEdlaunchToClose = false;  // WM_CLOSE was posted to at least one EDLaunch window
    bool edlaunchClosed = false;        // and EDLaunch was gone within closeWaitMs
};

// Polls until an outcome. Meant for a worker thread. `notify` (may be empty) is called from
// this thread at each WatchEvent. Never ends a process.
WatchOutcome watchLaunch(const WatchNames& names, const WatchTiming& timing, const std::atomic<bool>* cancel,
                         const std::function<void(WatchEvent)>& notify);

// Process ids whose executable is named `exeName` (case-insensitive). False when the process
// list could not be read, which is not the same as an empty answer.
bool pidsNamed(const std::wstring& exeName, std::vector<unsigned long>* pids);

}  // namespace edvr::installer
