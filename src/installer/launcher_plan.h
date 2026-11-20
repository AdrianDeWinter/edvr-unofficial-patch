// Applying an install from a short-lived elevated copy of the launcher.
//
// The launcher itself never runs elevated: the game must never start from an elevated
// process, and a window that can start the game has no business holding administrator
// rights. When a game folder needs them (it is usually under Program Files) the launcher
// writes what it decided to a plan file, starts a copy of itself with `runas` and
// `--apply-plan <file>`, waits, and reads the result file the copy wrote.
//
// The elevated copy shows no window and trusts nothing the file says beyond the choices it
// records. It re-verifies the manifest's signature and every payload file's hash itself,
// surveys the folder again, plans again and applies that plan.
//
// The file names no path the copy writes to or installs from. Any program running as the
// user could write a plan and start the copy, and the permission prompt would name this
// launcher, so a path in the file would let that program write anywhere as administrator.
// The copy works every path out from where the plan file is (planPathsFor): the result
// beside it, the manifest and the payload cache under the same data root, and the settings
// mirror from the game folder and that root. It installs only a signed download, never a
// --payload folder.
//
// Both files are one `key=value` per line in UTF-8, written atomically. A line it does not
// know, a value with a line break in it, or a missing key makes the reader refuse the file.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "detect.h"
#include "launcher_core.h"

namespace edvr::installer {

struct PlanFile {
    std::string  action;                 // install | repair | uninstall
    std::wstring dir;                    // the game folder
    std::wstring source;                 // how it was found ("Steam", "Epic", ...), for the planner
    std::string  scope;                  // both | vr | flat, or empty for the folder's own
    bool         keepSettings = true;
    bool         removeSettings = false;
    bool         restoreMirror = false;  // restore settings from the mirror first, as the user agreed
    std::string  manifestSha;            // the adopted manifest, 64 lower-case hex; empty only to uninstall
};

bool writePlanFile(const std::wstring& path, const PlanFile& plan, std::string* error);
bool readPlanFile(const std::wstring& path, PlanFile* plan, std::string* error);

// The paths the elevated copy uses, from where the plan file is. The plan must be
// <data root>\launcher\plan-<digits>-<digits>.txt, and neither it, its folder nor the data
// root may be a link (a junction or a symbolic link).
struct PlanPaths {
    std::wstring dataRoot;      // the folder that holds the launcher and payload folders
    std::wstring resultPath;   // <data root>\launcher\result-<the same digits>.txt
    std::wstring manifestsDir;  // <data root>\launcher\manifests
    std::wstring cacheDir;      // <data root>\payload
};
bool planPathsFor(const std::wstring& planPath, PlanPaths* out, std::string* error);

// The 64-hex name of a stored manifest (<sha>.json), or empty when the path is not one.
std::string manifestShaOf(const std::wstring& manifestPath);

struct ApplyOutcome {
    enum class Status { Done, NothingToDo, Blocked, Failed };
    Status status = Status::Failed;
    std::vector<std::string> lines;  // one per completed step, or the reasons
};

bool writeResultFile(const std::wstring& path, const ApplyOutcome& outcome, std::string* error);
bool readResultFile(const std::wstring& path, ApplyOutcome* outcome, std::string* error);

// The outcome as the report pane shows it.
std::string outcomeReport(const ApplyOutcome& outcome);

// What the elevated copy does. `keys` null means the release keys; the rig passes its own.
// Never throws, never shows anything: every failure is an outcome. Returns the process
// exit code for --apply-plan: 0 done or nothing to do, 1 blocked or failed, 2 a plan that
// cannot be read or is not where planPathsFor accepts it (no result is written for either).
// An optional already-discovered inventory supplies the store discovery boundary;
// null uses the machine's inventory, as the elevated launcher does.
int applyPlanFile(const std::wstring& planPath, const char* const* keys,
                  const std::vector<GameInstall>* installs = nullptr);

// How the elevated copy is started and waited for. The real one is ShellExecuteEx with
// `runas`; the rig starts the same program unelevated.
struct ElevatedLaunch {
    bool started = false;
    bool declined = false;     // the user said no at the UAC prompt
    unsigned long exitCode = 0;
    std::string error;
};
using ElevatedRunner = std::function<ElevatedLaunch(const std::wstring& exePath, const std::wstring& arguments)>;
ElevatedLaunch runWithUac(const std::wstring& exePath, const std::wstring& arguments);

struct ElevatedResult {
    bool declined = false;  // nothing changed, and the prompt was refused
    ApplyOutcome outcome;
    std::string error;      // when the copy never reported
};

// The unelevated half: writes the plan into `launcherDir` (which must be <data root>\launcher),
// runs `runner`, reads the result from where planPathsFor puts it, and deletes both files
// whatever happened.
ElevatedResult applyElevated(const std::wstring& exePath, const std::wstring& launcherDir, PlanFile plan,
                             const ElevatedRunner& runner);

}  // namespace edvr::installer
