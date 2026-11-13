#include "launcher_plan.h"

#include <windows.h>
#include <shellapi.h>

#include <map>
#include <set>

#include "../common/iniedit.h"
#include "apply.h"
#include "mirror.h"
#include "payload.h"
#include "plan.h"
#include "state.h"
#include "update_http.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

// 2: the file names no paths; the copy derives them (planPathsFor).
constexpr const char* kPlanVersion = "2";

bool setError(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

bool hasBreak(const std::string& value) { return value.find_first_of("\r\n") != std::string::npos; }

std::string yesNo(bool value) { return value ? "1" : "0"; }

// key=value lines into a map, refusing a repeat and a line with no '='.
bool parseLines(const std::string& text, std::map<std::string, std::string>* out, std::string* error) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) return setError(error, "a line has no key");
        const std::string key = line.substr(0, eq);
        if (key == "line") continue;  // result files repeat it; the caller reads them separately
        if (out->count(key)) return setError(error, "the key " + key + " is given twice");
        (*out)[key] = line.substr(eq + 1);
    }
    return true;
}

bool boolOf(const std::map<std::string, std::string>& m, const char* key, bool* out, std::string* error) {
    const auto found = m.find(key);
    if (found == m.end()) return setError(error, std::string("the key ") + key + " is missing");
    if (found->second != "0" && found->second != "1") return setError(error, std::string("the key ") + key + " is not 0 or 1");
    *out = found->second == "1";
    return true;
}

std::string textOf(const std::map<std::string, std::string>& m, const char* key) {
    const auto found = m.find(key);
    return found == m.end() ? std::string() : found->second;
}

ApplyOutcome failed(const std::string& why) {
    ApplyOutcome o;
    o.status = ApplyOutcome::Status::Failed;
    o.lines.push_back(why);
    return o;
}

const char* statusName(ApplyOutcome::Status s) {
    switch (s) {
        case ApplyOutcome::Status::Done: return "done";
        case ApplyOutcome::Status::NothingToDo: return "nothing";
        case ApplyOutcome::Status::Blocked: return "blocked";
        default: return "failed";
    }
}

bool isDigits(const std::wstring& text) {
    if (text.empty()) return false;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

// "plan-<digits>-<digits>.txt": the stamp between "plan-" and ".txt", or false.
bool planStampOf(const std::wstring& leaf, std::wstring* stamp) {
    const std::wstring head = L"plan-", tail = L".txt";
    if (leaf.size() <= head.size() + tail.size() || leaf.compare(0, head.size(), head) != 0 ||
        leaf.compare(leaf.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    const std::wstring middle = leaf.substr(head.size(), leaf.size() - head.size() - tail.size());
    const size_t dash = middle.find(L'-');
    if (dash == std::wstring::npos || !isDigits(middle.substr(0, dash)) || !isDigits(middle.substr(dash + 1))) return false;
    *stamp = middle;
    return true;
}

// True for a path that is missing or is a reparse point (a junction or a symbolic link).
bool missingOrLink(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// The plan's work, as outcome. Everything the elevated copy does after reading the file. Every
// path comes from `paths`, which planPathsFor worked out from where the file is.
ApplyOutcome runPlan(const PlanFile& file, const PlanPaths& paths, const char* const* keys) {
    if (file.action != "install" && file.action != "repair" && file.action != "uninstall")
        return failed("the plan names an action this launcher does not know");
    if (file.dir.empty()) return failed("the plan names no game folder");

    // The payload: the signed manifest the plan names by its hash, re-verified here with every file it lists.
    std::string error;
    if (!file.manifestSha.empty()) {
        OfflineClient offline;
        UpdaterSettings settings;
        settings.cacheDir = paths.cacheDir;
        settings.keys = keys;
        Updater updater(offline, settings);
        Manifest manifest;
        std::vector<PayloadFile> files;
        if (!updater.loadManifestFile(joinPath(paths.manifestsDir, widenUtf8(file.manifestSha)) + L".json", &manifest, &files, &error) ||
            !usePayloadFiles(manifest, files, &error))
            return failed("the downloaded files cannot be used: " + error);
    } else if (file.action != "uninstall") {
        return failed("the plan names no payload to install from");
    }

    GameInstall game;
    if (!describeDir(file.dir, file.source.empty() ? L"Folder you chose" : file.source, &game))
        return failed("there is no EliteDangerous64.exe in the game folder the plan names");
    const std::wstring mirrorDir = mirrorDirFor(game, paths.dataRoot);

    Survey survey = surveyTarget(game);
    const bool uninstall = file.action == "uninstall";
    ApplyOutcome outcome;

    // The restore the user agreed to in the window; it writes into the game folder, so it
    // is done here, with the rights the rest of the work has.
    if (file.restoreMirror && file.keepSettings && !uninstall) {
        const MirrorInfo mirror = readMirror(mirrorDir);
        const std::string scope = file.scope.empty() ? defaultScope(survey) : file.scope;
        if (!settingsToRestore(survey.game.dir, mirror, scope).empty()) {
            std::vector<std::string> notes;
            const bool restored = restoreFromMirror(survey.game.dir, mirror, scope, &notes);
            for (const std::string& n : notes) outcome.lines.push_back(n);
            if (!restored) outcome.lines.push_back("Not all of the saved settings could be restored; carrying on with what is in the folder.");
            survey = surveyTarget(game);
        }
    }

    Options options;
    options.scope = file.scope;
    options.keepSettings = file.keepSettings;
    options.removeSettings = file.removeSettings;
    options.repair = file.action == "repair";
    options.backupStamp = timestampName();
    options.nowUtc = utcNow();
    const Plan plan = uninstall ? planUninstall(survey, options, payloadInfo()) : planInstall(survey, options, payloadInfo());

    if (plan.blocked) {
        ApplyOutcome blocked;
        blocked.status = ApplyOutcome::Status::Blocked;
        for (const std::string& p : plan.problems) blocked.lines.push_back(p);
        if (blocked.lines.empty()) blocked.lines.push_back(planSummary(plan));
        return blocked;
    }
    if (plan.nothingToDo) {
        if (!uninstall) updateMirror(survey.game.dir, plan.backupDir, mirrorDir);
        outcome.status = ApplyOutcome::Status::NothingToDo;
        outcome.lines.push_back(planSummary(plan));
        return outcome;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);
    for (const std::string& line : result.done) outcome.lines.push_back(line);
    if (!result.ok) {
        outcome.status = ApplyOutcome::Status::Failed;
        outcome.lines.push_back(result.error);
        if (result.rolledBack) outcome.lines.push_back("Installed files and metadata were restored; recovery backups were kept.");
        return outcome;
    }
    if (!uninstall) {
        const MirrorResult m = updateMirror(survey.game.dir, plan.backupDir, mirrorDir);
        if (m.ok) {
            std::string saved;
            for (size_t i = 0; i < m.saved.size(); ++i) saved += (i ? ", " : "") + m.saved[i];
            outcome.lines.push_back("Mirrored " + saved + " to " + narrowUtf8(mirrorDir) + ".");
        }
    }
    outcome.status = ApplyOutcome::Status::Done;
    return outcome;
}

}  // namespace

bool writePlanFile(const std::wstring& path, const PlanFile& p, std::string* error) {
    const std::string dir = narrowUtf8(p.dir), source = narrowUtf8(p.source);
    for (const std::string* v : {&p.action, &p.scope, &dir, &source, &p.manifestSha}) {
        if (hasBreak(*v)) return setError(error, "a value in the plan has a line break in it");
    }
    std::string text;
    text += std::string("version=") + kPlanVersion + "\n";
    text += "action=" + p.action + "\n";
    text += "dir=" + dir + "\n";
    text += "source=" + source + "\n";
    text += "scope=" + p.scope + "\n";
    text += "keep_settings=" + yesNo(p.keepSettings) + "\n";
    text += "remove_settings=" + yesNo(p.removeSettings) + "\n";
    text += "restore_mirror=" + yesNo(p.restoreMirror) + "\n";
    text += "manifest=" + p.manifestSha + "\n";
    std::wstring why;
    if (!writeFileAtomic(path, text, &why)) return setError(error, "the plan file cannot be written");
    return true;
}

bool readPlanFile(const std::wstring& path, PlanFile* plan, std::string* error) {
    std::string text;
    if (!readFileBytes(path, &text, 1u << 16, error)) return false;
    std::map<std::string, std::string> m;
    if (!parseLines(text, &m, error)) return false;
    static const std::set<std::string> known = {"version",        "action",          "dir",            "source", "scope",
                                                "keep_settings",  "remove_settings", "restore_mirror", "manifest"};
    for (const auto& entry : m) {
        if (!known.count(entry.first)) return setError(error, "the plan has a key this launcher does not know: " + entry.first);
    }
    if (textOf(m, "version") != kPlanVersion) return setError(error, "the plan is a version this launcher does not read");
    PlanFile p;
    p.action = textOf(m, "action");
    p.dir = widenUtf8(textOf(m, "dir"));
    p.source = widenUtf8(textOf(m, "source"));
    p.scope = textOf(m, "scope");
    p.manifestSha = textOf(m, "manifest");
    if (!boolOf(m, "keep_settings", &p.keepSettings, error) || !boolOf(m, "remove_settings", &p.removeSettings, error) ||
        !boolOf(m, "restore_mirror", &p.restoreMirror, error))
        return false;
    if (p.action.empty() || p.dir.empty()) return setError(error, "the plan lacks an action or a folder");
    if (!p.manifestSha.empty() && !isLowerHex(p.manifestSha, 64)) return setError(error, "the plan's manifest is not a 64-character hash");
    if (!p.scope.empty() && p.scope != "both" && p.scope != "vr" && p.scope != "flat") return setError(error, "the plan names an unknown scope");
    *plan = std::move(p);
    return true;
}

bool writeResultFile(const std::wstring& path, const ApplyOutcome& outcome, std::string* error) {
    std::string text = std::string("status=") + statusName(outcome.status) + "\n";
    for (const std::string& line : outcome.lines) {
        std::string flat;
        for (char c : line) flat.push_back(c == '\r' || c == '\n' ? ' ' : c);
        text += "line=" + flat + "\n";
    }
    if (!writeFileAtomic(path, text)) return setError(error, "the result file cannot be written");
    return true;
}

bool readResultFile(const std::wstring& path, ApplyOutcome* outcome, std::string* error) {
    std::string text;
    if (!readFileBytes(path, &text, 1u << 20, error)) return false;
    ApplyOutcome out;
    bool haveStatus = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 7, "status=") == 0) {
            const std::string name = line.substr(7);
            if (name == "done") out.status = ApplyOutcome::Status::Done;
            else if (name == "nothing") out.status = ApplyOutcome::Status::NothingToDo;
            else if (name == "blocked") out.status = ApplyOutcome::Status::Blocked;
            else if (name == "failed") out.status = ApplyOutcome::Status::Failed;
            else return setError(error, "the result names a status this launcher does not know");
            haveStatus = true;
        } else if (line.compare(0, 5, "line=") == 0) {
            out.lines.push_back(line.substr(5));
        } else if (!line.empty()) {
            return setError(error, "the result has a line this launcher does not know");
        }
    }
    if (!haveStatus) return setError(error, "the result has no status");
    *outcome = std::move(out);
    return true;
}

std::string outcomeReport(const ApplyOutcome& outcome) {
    std::string text;
    for (const std::string& line : outcome.lines) text += "  " + line + "\r\n";
    switch (outcome.status) {
        case ApplyOutcome::Status::Done: text += "\r\nDone.\r\n"; break;
        case ApplyOutcome::Status::NothingToDo: text += "\r\nNothing needed changing.\r\n"; break;
        case ApplyOutcome::Status::Blocked: text += "\r\nNothing was changed: the plan is blocked.\r\n"; break;
        default: text += "\r\nThe install did not finish.\r\n"; break;
    }
    return text;
}

bool planPathsFor(const std::wstring& planPath, PlanPaths* out, std::string* error) {
    const size_t slash = planPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return setError(error, "the plan file's path names no folder");
    const std::wstring folder = planPath.substr(0, slash);
    std::wstring stamp;
    if (!planStampOf(planPath.substr(slash + 1), &stamp)) return setError(error, "the plan file is not named plan-<number>-<number>.txt");
    const size_t up = folder.find_last_of(L"\\/");
    if (up == std::wstring::npos || up == 0 || _wcsicmp(folder.substr(up + 1).c_str(), L"launcher") != 0)
        return setError(error, "the plan file is not in a launcher folder");
    const std::wstring root = folder.substr(0, up);
    // A link anywhere in the three would let the result, the cache or the mirror resolve somewhere else.
    for (const std::wstring* path : {&root, &folder, &planPath}) {
        if (missingOrLink(*path)) return setError(error, "the plan file, its folder or the folder above is missing or is a link");
    }
    PlanPaths p;
    p.dataRoot = root;
    p.resultPath = joinPath(folder, L"result-" + stamp + L".txt");
    p.manifestsDir = joinPath(folder, L"manifests");
    p.cacheDir = joinPath(root, L"payload");
    *out = std::move(p);
    return true;
}

std::string manifestShaOf(const std::wstring& manifestPath) {
    const size_t slash = manifestPath.find_last_of(L"\\/");
    const std::string leaf = narrowUtf8(slash == std::wstring::npos ? manifestPath : manifestPath.substr(slash + 1));
    if (leaf.size() != 64 + 5 || leaf.compare(64, 5, ".json") != 0 || !isLowerHex(leaf.substr(0, 64), 64)) return std::string();
    return leaf.substr(0, 64);
}

int applyPlanFile(const std::wstring& planPath, const char* const* keys) {
    // A plan that is not where the launcher writes plans, or cannot be read, has no result path to
    // report to, so the parent sees no result file.
    PlanPaths paths;
    std::string error;
    if (!planPathsFor(planPath, &paths, &error)) return 2;
    PlanFile plan;
    if (!readPlanFile(planPath, &plan, &error)) return 2;
    const ApplyOutcome outcome = runPlan(plan, paths, keys);
    writeResultFile(paths.resultPath, outcome, nullptr);
    return outcome.status == ApplyOutcome::Status::Done || outcome.status == ApplyOutcome::Status::NothingToDo ? 0 : 1;
}

ElevatedLaunch runWithUac(const std::wstring& exePath, const std::wstring& arguments) {
    ElevatedLaunch launch;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exePath.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        launch.declined = code == ERROR_CANCELLED;
        launch.error = launch.declined ? "the permission prompt was declined" : "Windows would not start the elevated copy";
        return launch;
    }
    launch.started = true;
    if (info.hProcess) {
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(info.hProcess, &exitCode);
        launch.exitCode = exitCode;
        CloseHandle(info.hProcess);
    }
    return launch;
}

ElevatedResult applyElevated(const std::wstring& exePath, const std::wstring& launcherDir, PlanFile plan,
                             const ElevatedRunner& runner) {
    ElevatedResult result;
    const std::wstring stamp = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::wstring planPath = joinPath(launcherDir, L"plan-" + stamp + L".txt");
    PlanPaths paths;
    auto cleanup = [&]() {
        DeleteFileW(planPath.c_str());
        if (!paths.resultPath.empty()) DeleteFileW(paths.resultPath.c_str());
    };
    if (!createDirectories(launcherDir) || !writePlanFile(planPath, plan, &result.error)) {
        if (result.error.empty()) result.error = "the launcher folder cannot be written";
        cleanup();
        return result;
    }
    // The result is read from where the copy will put it, which the copy works out the same way.
    if (!planPathsFor(planPath, &paths, &result.error)) {
        cleanup();
        return result;
    }
    DeleteFileW(paths.resultPath.c_str());  // never read a stale one
    const ElevatedLaunch launch = runner(exePath, L"--apply-plan " + quoteArg(planPath));
    if (!launch.started) {
        result.declined = launch.declined;
        result.error = launch.error;
        cleanup();
        return result;
    }
    std::string readError;
    if (!readResultFile(paths.resultPath, &result.outcome, &readError)) {
        result.error = "the elevated copy finished without reporting (exit code " + std::to_string(launch.exitCode) + "); nothing is known to have changed";
        result.outcome = failed(result.error);
    }
    cleanup();
    return result;
}

}  // namespace edvr::installer
