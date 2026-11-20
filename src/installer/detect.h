// Finding Elite Dangerous, however it was installed.
//
// The README's install table lists three paths and then says: if none of those
// match, find EliteDangerous64.exe yourself. That last sentence is the part
// people get wrong -- the Frontier launcher alone has moved its Products folder
// between %LOCALAPPDATA% and Program Files, Steam libraries live on whichever
// drive had room, and Epic writes its own manifests somewhere neither of the
// others look.
//
// So nothing here trusts a fixed path. Each store is asked where it put the
// game in its own terms (Steam's library index, Epic's manifests, the
// uninstall registry, the launcher's own folders), every answer is then
// CONFIRMED by finding EliteDangerous64.exe underneath it, and anything that
// cannot be confirmed is not offered. The user can always point at a folder
// themselves, and that path goes through exactly the same confirmation.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

struct GameInstall {
    std::wstring dir;        // the folder holding EliteDangerous64.exe
    std::wstring openvrDir;  // the folder holding the game's openvr_api.dll; empty if not found
    std::wstring source;     // "Steam", "Epic", "Frontier launcher", "Folder you chose"
    std::wstring product;    // the Products\ folder name, e.g. elite-dangerous-odyssey-64
    bool odyssey = false;    // Odyssey rather than the Horizons build
};

// Every supported install this machine can be shown to have, Odyssey first.
// Confirmed by the executable, deduplicated by path. Legacy installs are left out,
// because no build of this mod supports them, and counted in *legacyLeftOut so an
// empty list can say why instead of "not found". Browse can still reach one, and
// the planner then says why it refuses.
std::vector<GameInstall> findInstalls(size_t* legacyLeftOut = nullptr);

// Confirm the installations named by Epic's .item files in this directory.
std::vector<GameInstall> findEpicInstalls(const std::wstring& manifestDir);

// Confirm the installations in one Epic launcher journal (installations.jsonl).
// The current launcher writes that journal under the AppDataPath it records.
// A missing file is no installs. A journal that cannot be read as a version-1
// transaction log is no installs: a torn line is not a reason to keep the
// earlier lines. The executable check is the same one the .item reader uses.
std::vector<GameInstall> findEpicJournal(const std::wstring& journalPath);

// What to say when the only installs found were legacy ones: they are there, and why
// they are not offered. Ends without a line break.
const char* onlyLegacyInstallsText();

// `installs` without those whose EliteDangerous64.exe `isLegacyExe` says is legacy.
std::vector<GameInstall> withoutLegacyInstalls(const std::vector<GameInstall>& installs, bool (*isLegacyExe)(const std::wstring&));

// The folders Frontier's launcher may be installed in: what its installer recorded
// in the registry, then its default places. The store keeps the game under the
// user's AppData, well away from EDLaunch.exe, so a walk up from the game folder
// does not find it. May hold empty strings and folders that do not exist.
std::vector<std::wstring> frontierLauncherRoots();

// Does this folder hold EliteDangerous64.exe?
bool gameDirLooksRight(const std::wstring& dir);

// Build the record for a folder the user picked. `dir` may be the game folder
// itself or a launcher root above it; the first confirmed product is used.
// A matching store record supplies its source; otherwise `source` is kept.
bool describeDir(const std::wstring& dir, const std::wstring& source, GameInstall* out);

// The same resolution over an already discovered inventory, without scanning
// the machine again. Only an exact canonical product-folder match carries a source.
bool describeDirWithInstalls(const std::wstring& dir, const std::wstring& source,
                             const std::vector<GameInstall>& installs, GameInstall* out);

// Whichever of Openvr\win64, Openvr or the game folder holds the game's
// openvr_api.dll (or the openvr_api_orig.dll a previous install left). Empty
// when none of them do -- which is a fact the planner has to report, not
// paper over.
std::wstring findOpenvrDir(const std::wstring& gameDir);

// Is Elite Dangerous running, and does that stop us?
//
// Every file this installer touches is one the game holds open while it runs
// -- but only in the folder it was launched from. A machine with two installs
// (the Frontier launcher's and Steam's) plays one while the other is patched,
// and asking by executable NAME alone refused the second for the first one's
// sake.
enum class GameRunState {
    NotRunning,   // no EliteDangerous64.exe anywhere on this machine
    OtherFolder,  // running, and every copy of it came from some other install
    ThisFolder,   // running from the folder about to be written into
    // The process list could not be read (or not to its end), so "stopped" is
    // not something anything here can say. Not "not running": that reading
    // let a failed snapshot walk the installer into a folder the game had open.
    // A caller treats it as a refusal, like ThisFolder, and says why.
    Unknown,
};

// EliteDangerous64.exe, weighed against the folder about to be written into.
GameRunState gameRunState(const std::wstring& gameDir);

// The same question about any executable name. It takes the name rather than
// fixing it because that is the only way to test the answer: no build machine
// has Elite running, and every one of them has the test process itself.
GameRunState runStateOf(const wchar_t* exeName, const std::wstring& dir);

// runStateOf over a process snapshot somebody else took (a TH32CS_SNAPPROCESS
// handle, passed as void* so this header needs no windows.h). Takes ownership:
// the handle is closed. INVALID_HANDLE_VALUE or null -- CreateToolhelp32Snapshot
// having failed -- is Unknown, which is the one branch a test cannot reach by
// asking Windows for a snapshot.
GameRunState runStateOfSnapshot(void* snapshot, const wchar_t* exeName, const std::wstring& dir);

// Is there an NVIDIA graphics adapter in this machine, by DXGI's vendor id,
// software adapters skipped? NVIDIA's DLSS runtime is placed only where
// there is one. The first such adapter's name goes to `name` when given. A
// machine with an AMD integrated GPU beside an NVIDIA card counts.
bool nvidiaAdapterPresent(std::wstring* name);

// Path and text helpers shared with the rest of the installer.
std::wstring joinPath(const std::wstring& dir, const std::wstring& leaf);
std::wstring leafOf(const std::wstring& path);
bool         fileExists(const std::wstring& path);
bool         dirExists(const std::wstring& path);
std::wstring canonicalPath(const std::wstring& path);
std::string  toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

// The whole file, or an empty string. Text only: everything read this way is
// an ini, a manifest or a launcher's index.
std::string readTextFile(const std::wstring& path, size_t limit = 4u << 20);

}  // namespace edvr::installer
