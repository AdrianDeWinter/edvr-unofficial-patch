// The bits both faces of the installer share: the command line, the plain-text
// description of what is in a game folder, and the elevation hand-off.
//
// The window is the product; the command line exists because an installer that
// can only be driven by hand cannot be tested, cannot be scripted by whoever
// packages EDVR for a community, and cannot tell you what it WOULD do without
// doing it (--dry-run, which is how every case in tools/installer_test was
// first checked against a real folder).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "../common/install_scope.h"
#include "plan.h"

namespace edvr::installer {

struct AppArgs {
    enum class Act { None, Install, Repair, Uninstall, CollectLogs };

    Act          action = Act::None;
    std::wstring dir;                    // --dir, the game folder
    std::wstring store;                  // --store: canonical source for this exact --dir; empty means detect
    std::string  scope;                  // --scope both|vr|flat; empty means the folder's own, else both
    bool         keepSettings = true;    // --replace-settings
    bool         removeSettings = false; // --remove-settings (uninstall)
    bool         purge = false;          // --purge (clean uninstall: removes both INIs, logs, backups, install dir and mirror)
    bool         edhm2dMigoto = false;   // opt-in tailored 2DMigoto d3d11_edhm.dll placement
    bool         dryRun = false;         // --dry-run
    bool         convertProfile = false; // --convert-profile: accepted, and no longer needed
    bool         autorun = false;        // set on the elevated relaunch: already confirmed
    bool         help = false;
    bool         badArg = false;
    std::wstring badArgText;
    std::wstring userDataRoot;     // --user-data-root <dir>: the unelevated user's %LOCALAPPDATA%\EDUP, passed to the elevated copy

#ifdef EDUP_LAUNCHER
    // The launcher's own flags (docs\design-launcher-2026-11-07.md). The installer's target does not
    // define EDUP_LAUNCHER, so it neither has these fields nor accepts these flags.
    std::wstring payloadDir;       // --payload <dir>: install from an unpacked ed-up-<ver>.zip
    std::wstring dataRoot;         // --data-root <dir>: every launcher folder under here, nothing registered
    std::wstring planFile;        // --apply-plan <file>: the elevated copy's whole job
    std::wstring probeOpenXrLoader; // --probe-openxr <dll>: internal out-of-process OpenXR query helper
    std::string  probeOpenXrSha;    // --probe-openxr-sha <64-hex>: expected SHA-256 of loader DLL
    std::string  channel;          // --channel stable|rc, for this run
    bool         checkUpdate = false;  // --check-update
    bool         update = false;       // --update
    bool         unregister = false;   // --unregister
    bool         updated = false;      // --updated: started by a self-update (internal)
    bool         directDryRun = false; // --direct-dry-run: sign in, print the masked command line, start nothing
    bool         vr = false;           // --vr, for --direct-dry-run
    bool         novr = false;         // --novr, for --direct-dry-run
    bool         licenses = false;     // --licenses: print the third-party licence the launcher carries
    bool         closeAfterStart = false;  // --close-after-start: close once Elite has started, for this run
    bool         headless = false;     // --headless: start Elite from --dir with no window
    bool         frontierLogin = false; // --frontier-login: use Frontier account login on Steam direct launch
    std::vector<std::wstring> wrappedArgs; // trailing command args (e.g. Steam %command%) passed after --headless
#endif
};

AppArgs parseArgs(int argc, wchar_t** argv);
std::string usageText();

// Resolve only args.dir through the detection owner. An explicit store needs the
// exact product folder, and applies to that verified folder alone. The optional
// inventory keeps callers with an existing scan from scanning the machine again.
bool describeSelectedDir(const AppArgs& args, GameInstall* out,
                         const std::vector<GameInstall>* installs = nullptr);

Options optionsFor(const AppArgs& args, bool repair);

// The scope a run installs: the one asked for, else the folder's own, else both.
std::string scopeFor(const AppArgs& args, const Survey& survey);

// Elite's 3D mode, from the Settings.xml the game keeps under the Windows profile.
// Unknown when the file is missing or unreadable.
edvr::ModeRead eliteGameMode();

// What is in this folder right now, in the words the user needs: which halves
// are installed, which other mod is in the chain, whether the game's original
// runtime is safe.
std::string statusReport(const Survey& survey, const PayloadInfo& payload, const std::string& scope);

// Everything the plan will do, then everything the merge decided.
std::string planReport(const Plan& plan);

// Does this run need administrator rights for that folder?
bool needsElevationFor(const Survey& survey);

// Relaunch ourselves elevated, carrying the same arguments plus --autorun.
// Returns false if the user declined the UAC prompt.
bool relaunchElevated(const AppArgs& args);

bool isElevated();
// True only when running with a UAC split elevated token (TokenElevationTypeFull), so closing and
// reopening normally will run unelevated. False when UAC is disabled ("Never notify",
// TokenElevationTypeDefault) because every desktop process shares the same elevated token.
bool elevationBlocksPlay(bool tokenElevated, unsigned long elevationType);
bool isElevatedForPlay();

// Returns true when `mirrorDir` is also the mirror location for another discovered install in
// `knownInstalls`, so clean purge will not delete a shared mirror belonging to two folders.
bool mirrorSharedWithAnotherInstall(const GameInstall& target, const std::wstring& mirrorDir,
                                    const std::wstring& mirrorRoot,
                                    const std::vector<GameInstall>& knownInstalls);

// Cleans up artifacts outside the game directory after a --purge uninstall
// (any EDLaunch stand-in shim and, when `removeMirror` is true, the settings mirror folder),
// appending notes for what was removed. Shared by console, GUI, and elevated plan apply.
// Returns true if anything was removed.
bool purgeAfterUninstall(const std::wstring& gameDir, const std::wstring& mirrorDir,
                         std::vector<std::string>* notes, bool removeMirror = true);

int runConsole(const AppArgs& args);
int runGui(HINSTANCE instance, const AppArgs& args);

}  // namespace edvr::installer
