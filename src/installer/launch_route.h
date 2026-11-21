// Starting the game through EDLaunch, and watching until it is over.
//
// This is the route every store can use (docs\design-launcher-2026-11-07.md, "The EDLaunch
// route"):
//
//   Steam      steam://rungameid/359320 (steam://run/359320 under Wine)
//   Epic       com.epicgames.launcher://apps/<sandbox>%3A<catalog>%3A<artifact>?action=launch&silent=true
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

#include "../common/stereo_mode.h"

namespace edvr::installer {

enum class RouteKind { None, Steam, Epic, Frontier };

struct LaunchRoute {
    RouteKind kind = RouteKind::None;
    std::wstring uri;      // Steam and Epic
    std::wstring exe;      // Frontier: the EDLaunch.exe
    std::wstring args;     // EDLaunch's flags, which carry VR or flat
    std::wstring workDir;  // where EDLaunch.exe is
    // Frontier's route carries VR or flat. Steam's address does not. Epic's address does not
    // either, unless the skip-the-launcher checkbox starts that install's own EDLaunch.
    bool carriesMode = false;
    std::string why;       // when kind is None: why there is no Play button
};

inline constexpr const wchar_t* kSteamAppId = L"359320";
// Elite Dangerous on the Epic catalog. The launch address is the sandbox, the
// catalog item and the artifact, the same kind of fixed identity as Steam's app
// id. The artifact alone is the old form of that address.
inline constexpr const wchar_t* kEpicNamespaceId = L"3db17abfd650423f993291624b1b2ac1";
inline constexpr const wchar_t* kEpicCatalogId = L"9b8ee44da2e84b139c7444ab002cbc35";
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
// `direct` is the "skip the launcher" checkbox. For an Epic install the address is
// unchanged: Epic has to start the program, because only then does it pass the exchange
// code. What changes is that EDLaunch.exe is our stand-in, which signs in and starts
// Elite itself. Every other source ignores `direct`.
LaunchRoute routeFor(const std::wstring& source, const std::wstring& gameDir, bool vr,
                     const std::vector<std::wstring>* frontierRoots = nullptr, bool direct = false);

// Frontier's EDLaunch.exe, kept beside the stand-in so a game update can be told from ours
// and so uninstall can put it back. The mode file is "vr" or "flat".
inline constexpr const wchar_t* kEdLaunchOrigName = L"EDLaunch_orig.exe";
inline constexpr const wchar_t* kEdLaunchModeName = L"edvr_launch_mode.txt";
inline constexpr const wchar_t* kEdLaunchMarkerName = L"edvr_edlaunch.txt";

// The command line that runs the saved EDLaunch. `commandLine` is the full line Epic used,
// including the leading exe. /autorun /autoquit /edo are added when they are not already
// there, and so is /vr or /novr when `chooseMode` is set.
std::wstring edLaunchShimCommand(const std::wstring& origExe, const std::wstring& commandLine, bool vr,
                                 bool chooseMode);

// -1 when this process is not standing in for EDLaunch.exe. Otherwise the process is the
// stand-in: it signs in to Epic and Frontier, starts Elite itself, and the return value
// is this process's exit code. It exits once Elite is running.
int runEdLaunchShimIfInvoked();

// Put `selfExe` where Epic starts the game, keeping Frontier's binary as EDLaunch_orig.exe.
// A missing self file does nothing and returns true. False leaves the folder as it was.
bool installEdLaunchShim(const std::wstring& selfExe, const std::wstring& gameDir, std::string* error);
// Put Frontier's binary back. True when there was nothing to put back.
bool removeEdLaunchShim(const std::wstring& gameDir, std::string* error);
// The stand-in reads this on the next start Epic makes.
bool writeEdLaunchMode(const std::wstring& gameDir, bool vr);

struct RouteOps {
    std::function<bool(const std::wstring& uri)> openUri;
    std::function<bool(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir)> spawn;
    bool underWine = false;  // Steam's URI must reach the native client, not Proton's restart event.
    // True when openUri spawns a wrapped executable (e.g. Steam %command%) that already receives /vr or /novr.
    bool wrappedCarriesMode = false;
};
RouteOps realRouteOps();

// The sentence for a start refused because the launcher is elevated. Direct launch says the same.
const char* elevatedRefusal();

// Starts the route. Refuses when `elevated` is true: the game must never start from an
// elevated process. `error` says why on false.
bool startRoute(const LaunchRoute& route, const RouteOps& ops, bool elevated, std::string* error);

// Default path to Elite's Settings.xml under %LOCALAPPDATA% (empty if LocalAppData cannot be resolved).
std::wstring defaultEliteSettingsXmlPath();

// Prepares Elite's Settings.xml for a launch (`vr` = true for VR, false for flat) before starting the
// store client. Refuses if `gameExeName` (when non-empty) is already running. Keeps a one-time
// Settings.xml.ed-up-original (the untouched original before ED-UP first writes) plus a rolling
// Settings.xml.bak in `backupDir`. For flat, writes 0 into <StereoscopicMode> (inserting the tag
// before </GraphicsOptions> if absent). For VR, requires `preferredVrMode` != NotSet (refuses without
// modifying the file when NotSet) and writes that mode. A parse failure writes nothing.
bool prepareSettingsXmlForLaunch(const std::wstring& settingsXmlPath, const std::wstring& backupDir, bool vr,
                                 edvr::VrHeadsetMode preferredVrMode, std::string* error,
                                 const std::wstring& gameExeName = L"EliteDangerous64.exe");

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
