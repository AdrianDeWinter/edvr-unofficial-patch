#include "launcher_main.h"

#ifdef EDUP_LAUNCHER

#include <windows.h>
#include <objbase.h>

#include <cwctype>
#include <functional>
#include <vector>

#include "direct_launch.h"
#include "frontier_login.h"
#include "launch_route.h"
#include "launcher_plan.h"
#include "launcher_register.h"
#include "launcher_state.h"
#include "logbundle.h"
#include "mirror.h"
#include "probe.h"
#include "product_title.h"
#include "selfupdate.h"
#include "update_http.h"
#include "update_keys.h"
#include "update_manifest.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

std::string g_startupNote;

std::wstring selfPath() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    return std::wstring(buffer, n);
}

bool samePath(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Text for a console, or, with none (--unregister run from Windows' list of programs), collected
// for one message box at the end.
class Output {
public:
    explicit Output(bool console) : console_(console) {}

    void line(const std::string& text) {
        if (!console_) {
            collected_ += text + "\n";
            return;
        }
        const std::string withBreak = text + "\r\n";
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
        DWORD mode = 0, written = 0;
        if (GetConsoleMode(h, &mode)) {
            const std::wstring w = fromUtf8(withBreak);
            WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
        } else {
            WriteFile(h, withBreak.data(), static_cast<DWORD>(withBreak.size()), &written, nullptr);
        }
    }

    // Text exactly as given, with no line end added: --licenses prints a file's bytes.
    void raw(const std::string& text) {
        if (!console_) {
            collected_ += text;
            return;
        }
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
        DWORD mode = 0, written = 0;
        if (GetConsoleMode(h, &mode)) {
            const std::wstring w = fromUtf8(text);
            WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
        } else {
            WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
    }

    void finish() {
        if (!console_ && !collected_.empty())
            MessageBoxW(nullptr, fromUtf8(collected_).c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
    }

private:
    bool console_;
    std::string collected_;
};

RegisterTargets targetsFor(const LauncherRoots& roots, const std::wstring& exe) {
    RegisterTargets t;
    t.exePath = exe;
    t.launcherDir = roots.launcherDir();
    t.payloadDir = roots.cacheDir();
    t.version = EDUP_LAUNCHER_VERSION;
    // Only the real root touches the Start menu and HKCU. A --data-root run is a trial, and its
    // unregister must never remove the entry of the launcher the user really has.
    if (roots.canonical) {
        t.startMenuDir = defaultStartMenuDir();
        t.registryKey = defaultUninstallKey();
    }
    return t;
}

std::string signedBy(const UpdateCheck& check) {
    if (check.manifestBytes.empty() || check.signatureText.empty()) return "none";
    int index = 0;
    for (const char* const* key = kUpdatePublicKeys; *key; ++key, ++index) {
        const char* one[2] = {*key, nullptr};
        Manifest scratch;
        if (loadVerifiedManifest(check.manifestBytes, check.signatureText, one, &scratch, nullptr))
            return index == 0 ? "the main key" : "the recovery key";
    }
    return "none";
}

const char* outcomeName(UpdateOutcome o) {
    switch (o) {
        case UpdateOutcome::UpToDate: return "up to date";
        case UpdateOutcome::Available: return "update available";
        case UpdateOutcome::Refused: return "refused";
        default: return "skipped";
    }
}

int runUpdateCommand(const AppArgs& args, Output& out) {
    const LauncherRoots roots = launcherRootsFor(args);
    createDirectories(roots.launcherDir());
    WinHttpClient http(false, EDUP_LAUNCHER_VERSION);
    UpdaterSettings settings;
    settings.launcherDir = roots.launcherDir();
    settings.cacheDir = roots.cacheDir();
    settings.channel = args.channel;
    settings.launcherSha256 = sha256File(selfPath());
    LauncherState state;
    state.load(joinPath(roots.launcherDir(), L"launcher.ini"), nullptr);
    Updater updater(http, settings);
    const UpdateCheck check = updater.check();

    out.line("Channel:         " + (args.channel.empty() ? state.channel() : args.channel));
    out.line("Release:         " + (check.tag.empty() ? std::string("(none)") : check.tag));
    out.line("Manifest sha256: " + (check.manifestSha256.empty() ? std::string("(not fetched)") : check.manifestSha256));
    out.line("Signed by:       " + signedBy(check));
    out.line(std::string("Outcome:         ") + outcomeName(check.outcome));
    if (!check.message.empty()) out.line("                 " + check.message);
    if (!check.detail.empty()) out.line("                 " + check.detail);

    switch (check.outcome) {
        case UpdateOutcome::Refused: return 3;
        case UpdateOutcome::Skipped: return 4;
        case UpdateOutcome::UpToDate: return 0;
        default: break;
    }
    if (!args.update) return 0;

    if (check.launcherNeedsUpdate)
        out.line("The release carries a newer launcher. The window replaces the launcher; --update leaves it alone.");
    std::string lastAsset, error;
    const DownloadProgress progress = [&](const std::string& name, unsigned long long, unsigned long long) {
        if (name == lastAsset) return;
        lastAsset = name;
        out.line("Downloading " + name);
    };
    if (!updater.downloadUpdate(check, nullptr, progress, &error)) {
        out.line("The download did not finish: " + error);
        return 1;
    }
    PayloadCache cache(roots.cacheDir());
    for (const ManifestFile& f : check.manifest.files) {
        const bool ok = cache.hasVerified(f.sha256, f.size);
        out.line(std::string(ok ? "  verified " : "  NOT VERIFIED ") + f.path + "  " + f.sha256);
        if (!ok) return 1;
    }
    if (!updater.commitManifest(check, &error)) {
        out.line("The manifest could not be adopted: " + error);
        return 1;
    }
    out.line("Adopted manifest " + check.manifestSha256 + " (" + check.tag + "). No game folder and no exe was touched.");
    return 0;
}

// The text of min-ed-launcher's licence, which gen_installer_rc.py embeds as RCDATA 120.
constexpr int kLicenseResource = 120;

int runLicensesCommand(Output& out) {
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(kLicenseResource), RT_RCDATA);
    HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = found ? SizeofResource(nullptr, found) : 0;
    if (!data || size == 0) {
        out.line("This launcher carries no licence text. That is a build fault: please report it.");
        return 1;
    }
    out.raw(std::string(static_cast<const char*>(data), size));
    return 0;
}

// --direct-dry-run on the real network and the real Steam client. The elevated copy is refused before
// anything is contacted.
int runDirectDryRun(const AppArgs& args, Output& out) {
    if (isElevated()) {
        out.line(elevatedRefusal());
        return 2;
    }
    WinHttpClient http(false, EDUP_LAUNCHER_VERSION);
    DirectEnv env;
    env.http = &http;
    // A Frontier-store install uses the login the window stored, and never asks for one here.
    env.loginPath = joinPath(launcherRootsFor(args).launcherDir(), kFrontierLoginFile);
    std::vector<std::string> lines;
    const int code = directDryRunLines(args, env, &lines);
    for (const std::string& line : lines) out.line(line);
    return code;
}

int runUnregisterCommand(const AppArgs& args, Output& out) {
    const LauncherRoots roots = launcherRootsFor(args);
    const std::wstring self = selfPath();
    UnregisterResult result;
    std::string error;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ok = unregisterLauncher(targetsFor(roots, self), self, &result, &error);
    CoUninitialize();
    if (!ok) {
        out.line("Nothing was removed: " + error);
        return 1;
    }
    for (const std::string& note : result.notes) out.line(note);
    if (result.exeRemains) scheduleSelfRemoval(self, roots.launcherDir());
    return 0;
}

}  // namespace

int directDryRunLines(const AppArgs& args, DirectEnv env, std::vector<std::string>* lines) {
    if (args.dir.empty() || args.vr == args.novr) {
        lines->push_back("--direct-dry-run needs --dir <the game's product folder> and exactly one of --vr and --novr.");
        return 2;
    }
    env.progress = [lines](const std::string& line) { lines->push_back(line); };
    // Steam keeps its games under steamapps\common; anything else is taken for Frontier's own store.
    std::wstring lowered = args.dir;
    for (wchar_t& c : lowered) c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
    DirectRequest request;
    request.productDir = args.dir;
    request.vr = args.vr;
    request.frontier = lowered.find(L"\\steamapps\\") == std::wstring::npos;
    // A dry run never asks and never changes the stored login, whatever the caller handed in.
    env.prompts = FrontierPrompts();
    lines->push_back(request.frontier ? "Dry run for an install from Frontier's own store, with the stored login."
                                      : "Dry run for a Steam install, with the Steam login.");
    const DirectPlan plan = planDirectLaunch(env, request);

    size_t start = 0;
    while (start < plan.detail.size()) {
        size_t end = plan.detail.find('\n', start);
        if (end == std::string::npos) end = plan.detail.size();
        std::string line = plan.detail.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines->push_back(maskSecrets(line, plan.secrets));
        start = end + 1;
    }
    if (!plan.start) {
        // A stop is a whole sentence of its own, not a fallback: the dry run starts nothing either way.
        lines->push_back(maskSecrets(plan.stopped ? plan.reason : fallbackLine(plan.reason, request.frontier), plan.secrets));
        return 3;
    }
    lines->push_back("Dry run: nothing was started. The command line would be:");
    lines->push_back("    " + maskSecrets(narrowUtf8(plan.exe) + " " + narrowUtf8(plan.args), plan.secrets));
    return 0;
}

LauncherRoots launcherRootsFor(const AppArgs& args) { return launcherRoots(args.dataRoot); }

std::wstring launcherMirrorRoot(const AppArgs& args) { return launcherRootsFor(args).dataRoot; }

bool launcherPreparePayload(const AppArgs& args, std::string* note) {
    const PayloadLoad load = loadLauncherPayload(launcherRootsFor(args), args.payloadDir, nullptr);
    if (load.ok) return true;
    *note = load.firstRun ? "this launcher has no downloaded release yet. Run it with --update, or point --payload at an unpacked ed-up-<version>.zip."
                          : load.note;
    return false;
}

bool launcherHasCommand(const AppArgs& args) {
    return args.checkUpdate || args.update || args.unregister || args.directDryRun || args.licenses;
}

int runLauncherCommand(const AppArgs& args, bool haveConsole) {
    Output out(haveConsole);
    int code;
    if (args.licenses) code = runLicensesCommand(out);
    else if (args.directDryRun) code = runDirectDryRun(args, out);
    else if (args.unregister) code = runUnregisterCommand(args, out);
    else code = runUpdateCommand(args, out);
    out.finish();
    return code;
}

int runApplyPlanFile(const AppArgs& args) { return applyPlanFile(args.planFile, nullptr); }

const std::string& launcherStartupNote() { return g_startupNote; }

bool launcherStartup(const AppArgs& args, int* exitCode) {
    *exitCode = 0;
    const LauncherRoots roots = launcherRootsFor(args);
    if (!roots.canonical) return true;

    const std::wstring self = selfPath();

    // One window. A launcher started by a self-update waits a moment for the old one to leave.
    if (!takeInstanceMutex(args.updated ? 8000 : 0)) {
        bringRunningLauncherForward();
        return false;
    }
    // Only the launcher that holds the mutex clears .old and .new: a second start must not delete the
    // .new a running launcher has just downloaded and verified and is about to swap in.
    removeLauncherLeftovers(self);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring home = roots.exePath();
    bool carryOn = true;
    if (runningUnderWine()) {
        // Run in place: no copy to the canonical home, no registerLauncher, no restart. A process the launcher
        // starts may be ended with Steam's runtime container when the first one exits (unverified), and a Start
        // menu entry inside a Proton prefix serves nobody.
    } else if (!samePath(self, home)) {
        // Copy there, unless the copy there is already newer, then run that one.
        const bool newerThere = fileExists(home) && compareVersions(narrowUtf8(fileVersionOf(home)), EDUP_LAUNCHER_VERSION) > 0;
        const bool ready = newerThere || (createDirectories(roots.launcherDir()) && CopyFileW(self.c_str(), home.c_str(), FALSE));
        if (!ready) {
            g_startupNote = "Could not copy the launcher to " + narrowUtf8(home) + ", so it is running from where it is and is not registered.";
        } else {
            std::string ignored;
            registerLauncher(targetsFor(roots, home), &ignored);
            releaseInstanceMutex();
            if (startLauncherProcess(home, restartArguments(false))) {
                carryOn = false;
            } else {
                takeInstanceMutex(0);
                g_startupNote = "Could not start the copy at " + narrowUtf8(home) + ", so it is running from where it is.";
            }
        }
    } else {
        std::string ignored;
        registerLauncher(targetsFor(roots, home), &ignored);  // refreshed each start: cheap, and repairs a deleted shortcut
    }
    CoUninitialize();
    return carryOn;
}

}  // namespace edvr::installer

#endif  // EDUP_LAUNCHER
