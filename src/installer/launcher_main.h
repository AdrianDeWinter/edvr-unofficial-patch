// The launcher's command line and start-up, on top of app.h. Compiled only with EDUP_LAUNCHER.
#pragma once

#ifdef EDUP_LAUNCHER

#include <string>
#include <vector>

#include "app.h"
#include "direct_launch.h"
#include "launcher_core.h"
#include "launch_route.h"

namespace edvr::installer {

// The roots this run uses: --data-root, else the real %LOCALAPPDATA%\EDVR.
LauncherRoots launcherRootsFor(const AppArgs& args);

// Where the settings mirror folders go: the data root, so --data-root keeps them out of the
// user's real folder too.
std::wstring launcherMirrorRoot(const AppArgs& args);

// Loads the payload the run installs from (--payload, else the adopted manifest). True when
// there is one; otherwise `note` says what to do.
bool launcherPreparePayload(const AppArgs& args, std::string* note);

// The commands that need no window: --check-update, --update, --unregister, --direct-dry-run and
// --licenses. Prints through
// the console when there is one, and otherwise (--unregister from Windows' list of programs) in
// one message box at the end. Returns the exit code.
bool launcherHasCommand(const AppArgs& args);
int runLauncherCommand(const AppArgs& args, bool haveConsole);
// --headless: starts Elite from args.dir (default: the folder this exe is in) through Play's route, with no window. 0 when Elite started, 1 when it
// did not, 2 for a bad command line. Waits for Elite to exit, or for the start with --close-after-start.
int runHeadless(const AppArgs& args, bool haveConsole);

class LauncherSession;
// Starts the selected install through headless Play's route. The caller has checked elevation and running games;
// supplying the route operations and watcher names keeps the same path usable without starting a real store.
void beginHeadlessPlay(LauncherSession& session, const GameInstall& game, bool vr, const RouteOps& ops,
                       const WatchNames& names, const WatchTiming& timing);

// --direct-dry-run over `env` (its http and seams): the lines to print and the exit code, 0 for a plan, 3
// for a fallback and 2 for a command line that is not usable. Starts nothing; every token, the Steam ticket
// and the machine ID are replaced by <masked> in the lines. runLauncherCommand makes the real environment.
// `installs`, when supplied, is a discovery snapshot instead of a new scan.
int directDryRunLines(const AppArgs& args, DirectEnv env, std::vector<std::string>* lines,
                      const std::vector<GameInstall>* installs = nullptr);

// --apply-plan: the elevated copy. No window, no console; returns the exit code.
int runApplyPlanFile(const AppArgs& args);

// Before any window: remove the leftovers of a self-update, keep to one window per user, and
// when running from somewhere other than %LOCALAPPDATA%\EDVR\launcher, copy there, register and
// start that copy. False means this process is done and `exitCode` is what to return.
// Does nothing, and returns true, under --data-root.
bool launcherStartup(const AppArgs& args, int* exitCode);

// What start-up wants the window to say (a failed copy, say); empty when there is nothing.
const std::string& launcherStartupNote();

}  // namespace edvr::installer

#endif  // EDUP_LAUNCHER
