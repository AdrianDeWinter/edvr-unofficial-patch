#include "launcher_main.h"

#ifdef EDUP_LAUNCHER

#include <windows.h>
#include <objbase.h>

#include <condition_variable>
#include <cwctype>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "detect.h"
#include "direct_launch.h"
#include "frontier_login.h"
#include "launch_route.h"
#include "launcher_core.h"
#include "launcher_plan.h"
#include "launcher_register.h"
#include "launcher_session.h"
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
#include "vr_headset.h"

namespace edvr::installer {
namespace {

std::string g_startupNote;

std::wstring selfPath() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    return std::wstring(buffer, n);
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
    std::set<std::string> verified;
    std::string optInWarning;
    if (!updater.downloadUpdate(check, nullptr, progress, &error, &verified, &optInWarning)) {
        out.line("The download did not finish: " + error);
        return 1;
    }
    if (!optInWarning.empty()) {
        out.line("Warning: optional EDHM 2DMigoto asset was skipped (" + optInWarning + "); continuing with core ED-UP.");
    }
    const bool wantOptIn = state.edhm2dMigoto();
    PayloadCache cache(roots.cacheDir());
    for (const ManifestFile& f : check.manifest.files) {
        if (!wantOptIn && !f.when.option.empty()) continue;
        const bool ok = verified.find(f.sha256) != verified.end() ? cache.hasSized(f.sha256, f.size)
                                                                  : cache.hasVerified(f.sha256, f.size);
        if (!ok && !f.when.option.empty()) {
            out.line("  WARNING (opt-in unavailable) " + f.path + "  " + f.sha256);
            continue;
        }
        out.line(std::string(ok ? "  verified " : "  NOT VERIFIED ") + f.path + "  " + f.sha256);
        if (!ok) return 1;
    }
    if (!updater.commitManifest(check, &error, &verified)) {
        out.line("The manifest could not be adopted: " + error);
        return 1;
    }
    out.line("Adopted manifest " + check.manifestSha256 + " (" + check.tag + "). No game folder and no exe was touched.");
    return 0;
}

// The third-party licence texts embedded by gen_installer_rc.py:
//   120: min-ed-launcher (MIT, direct launch)
//   121: EDHM_2DMigoto / 3Dmigoto / XXMI / PCRE2 / DirectXTK / EDHM summary & source offer
//   122: GNU General Public License v3.0 full text (for optional EDHM_2DMigoto runtime)
constexpr int kLicenseResource = 120;
constexpr int kEdhmLicenseResource = 121;
constexpr int kEdhmGplResource = 122;

bool loadRcText(int id, std::string* out) {
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = found ? SizeofResource(nullptr, found) : 0;
    if (!data || size == 0) return false;
    out->assign(static_cast<const char*>(data), size);
    return true;
}

int runLicensesCommand(Output& out) {
    std::string minEd;
    if (!loadRcText(kLicenseResource, &minEd)) {
        out.line("This launcher carries no licence text. That is a build fault: please report it.");
        return 1;
    }
    out.raw(minEd);
    std::string edhm;
    if (loadRcText(kEdhmLicenseResource, &edhm)) {
        if (!minEd.empty() && minEd.back() != '\n') out.raw("\n");
        out.raw("\n------------------------------------------------------------------------\n\n");
        out.raw(edhm);
    }
    std::string gpl;
    if (loadRcText(kEdhmGplResource, &gpl)) {
        if (!edhm.empty() && edhm.back() != '\n') out.raw("\n");
        out.raw("\n------------------------------------------------------------------------\n\n");
        out.raw(gpl);
    }
    return 0;
}

// --direct-dry-run on the real network and the real Steam client. The elevated copy is refused before
// anything is contacted.
int runDirectDryRun(const AppArgs& args, Output& out) {
    if (isElevatedForPlay()) {
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

// --headless: Play with no window. The session runs as the window runs it: its workers post their
// jobs to a queue, and the jobs run here, on this thread, so the session's one-thread rule holds.
class HeadlessQueue {
public:
    void push(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        ready_.notify_one();
    }

    std::function<void()> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this]() { return !jobs_.empty(); });
        std::function<void()> job = std::move(jobs_.front());
        jobs_.pop_front();
        return job;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
};

// Store discovery is authoritative. An unregistered folder keeps the command line's previous
// Steam-path/Frontier fallback; headless Play and the direct dry run share that fallback.
std::wstring commandSource(const GameInstall& game) {
    if (!game.source.empty() && _wcsicmp(game.source.c_str(), L"Folder you chose") != 0) return game.source;
    const std::wstring& dir = game.dir;
    std::wstring lowered = dir;
    for (wchar_t& c : lowered) c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
    return lowered.find(L"\\steamapps\\") == std::wstring::npos ? L"Frontier launcher" : L"Steam";
}

// Infers the game folder from a wrapped command (e.g. Steam %command% passing "...\EDLaunch.exe").
std::wstring wrappedCommandDir(const std::vector<std::wstring>& wrapped) {
    if (wrapped.empty()) return std::wstring();
    const std::wstring& exe = wrapped.front();
    const size_t slash = exe.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);
}

// Builds RouteOps when invoked as a Steam %command% wrapper: instead of opening
// steam://rungameid/359320 (which would re-invoke %command% in a loop), spawn the wrapped exe directly.
// Because /vr or /novr is passed on the wrapped command line, wrappedCarriesMode is set so VR does not
// require a separate Settings.xml vr_3d_mode selection. If a wrapped command was given but its exe is
// missing, fail closed instead of falling back to steam://rungameid (which would loop).
RouteOps headlessRouteOps(const std::vector<std::wstring>& wrapped, bool vr) {
    RouteOps ops = realRouteOps();
    if (wrapped.empty()) return ops;
    const std::wstring wrappedExe = wrapped.front();
    ops.wrappedCarriesMode = true;
    if (!fileExists(wrappedExe)) {
        ops.openUri = [](const std::wstring&) { return false; };
        return ops;
    }
    std::wstring extra;
    bool hasMode = false;
    for (size_t i = 1; i < wrapped.size(); ++i) {
        if (_wcsicmp(wrapped[i].c_str(), L"/vr") == 0 || _wcsicmp(wrapped[i].c_str(), L"/novr") == 0) hasMode = true;
        if (!extra.empty()) extra += L' ';
        extra += quoteArg(wrapped[i]);
    }
    if (!hasMode) {
        if (!extra.empty()) extra += L' ';
        extra += vr ? L"/vr" : L"/novr";
    }
    const std::wstring workDir = wrappedCommandDir(wrapped);
    auto baseSpawn = ops.spawn;
    ops.openUri = [baseSpawn, wrappedExe, extra, workDir](const std::wstring&) {
        return baseSpawn && baseSpawn(wrappedExe, extra, workDir);
    };
    return ops;
}

int headlessPlay(const AppArgs& args, Output& out) {
    if (args.vr && args.novr) {
        out.line("--headless cannot take both --vr and --novr.");
        return 2;
    }
    if (isElevatedForPlay()) {
        out.line(elevatedRefusal());
        return 2;
    }
    // Optional pre-launch update check when --headless is paired with --update.
    // Offline/skipped checks (exit 4) do not prevent installing an adopted payload or launching Elite.
    if (args.update) {
        const int updateCode = runUpdateCommand(args, out);
        if (updateCode == 3) return updateCode;  // refused manifest
        if (args.action != AppArgs::Act::None && updateCode != 0 && updateCode != 4) return updateCode;
    }
    if (!args.wrappedArgs.empty() && !fileExists(args.wrappedArgs.front())) {
        out.line("Wrapped command executable not found: " + narrowUtf8(args.wrappedArgs.front()));
        return 2;
    }
    // Without --dir, check a wrapped executable's folder (Steam %command%) first, then this exe's folder.
    std::wstring dir = args.dir;
    if (dir.empty()) dir = wrappedCommandDir(args.wrappedArgs);
    if (dir.empty()) {
        const std::wstring self = selfPath();
        const size_t slash = self.find_last_of(L'\\');
        dir = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
    }
    AppArgs selectedArgs = args;
    selectedArgs.dir = dir;
    GameInstall game;
    if (!describeSelectedDir(selectedArgs, &game)) {
        out.line("No EliteDangerous64.exe under " + narrowUtf8(dir.empty() ? args.dir : dir) +
                 ". Put the launcher in the game's product folder, or pass --dir with it.");
        return 2;
    }
    // When --headless is paired with --install or --repair, apply the patch before launching Elite.
    if (args.action == AppArgs::Act::Install || args.action == AppArgs::Act::Repair) {
        AppArgs installArgs = selectedArgs;
        installArgs.dir = game.dir;
        installArgs.headless = false;
        const int installCode = runConsole(installArgs);
        if (installCode != 0) return installCode;
    }
    std::vector<unsigned long> running;
    if (!pidsNamed(L"EliteDangerous64.exe", &running)) {
        out.line("Windows would not list the running programs, so the launcher cannot tell whether Elite is already running. Nothing was started.");
        return 1;
    }
    if (!running.empty()) {
        out.line("Elite is already running. Nothing was started.");
        return 1;
    }

    // Decide VR vs flat when neither --vr nor --novr was given: honor a single-mode install scope,
    // else the remembered mode in launcher.ini, else Elite's Settings.xml.
    bool useVr = args.vr;
    if (!args.vr && !args.novr) {
        const LauncherRoots roots = launcherRootsFor(args);
        if (roots.canonical) migrateLegacyDataRoot(roots.dataRoot);
        const Survey survey = surveyTarget(game, mirrorDirFor(game, roots.dataRoot));
        const std::string scope = installedScope(survey);
        if (scope == "flat") {
            useVr = false;
        } else if (scope == "vr") {
            useVr = true;
        } else {
            LauncherState state;
            std::string remembered;
            if (state.load(joinPath(launcherRootsFor(args).launcherDir(), L"launcher.ini"), nullptr))
                remembered = state.mode(game.dir);
            if (remembered == "flat") useVr = false;
            else if (remembered == "vr") useVr = true;
            else {
                const edvr::ModeRead mode = eliteGameMode();
                useVr = !(mode.known && mode.mode == edvr::GameMode::Flat);
            }
        }
    }

    // Declared before the session, so the workers can post into it until the session is gone.
    HeadlessQueue queue;
    bool closeRequested = false;
    SessionEnv env;
    env.roots = launcherRootsFor(args);
    env.closeAfterStartArg = args.closeAfterStart;
    env.frontierLoginArg = args.frontierLogin;
    env.post = [&queue](std::function<void()> job) { queue.push(std::move(job)); };
    // The window's close: here it ends the wait. The lines are printed after each job, not from a callback.
    env.closeWindow = [&closeRequested]() { closeRequested = true; };
    const std::wstring settingsBackupDir = joinPath(env.roots.launcherDir(), L"backups");
    const std::wstring gameDirForXml = game.dir;
    auto lastSettingsSnapshot = std::make_shared<SettingsXmlSnapshot>();
    env.detectVrMode = [](const std::wstring& dirForXml) {
        return detectVrHeadsetModeFromSettingsXml(defaultEliteSettingsXmlPath(dirForXml));
    };
    env.prepareLaunchMode = [settingsBackupDir, gameDirForXml, lastSettingsSnapshot](
                                bool vr, edvr::VrHeadsetMode preferredVrMode, std::string* error) {
        return prepareSettingsXmlForLaunch(defaultEliteSettingsXmlPath(gameDirForXml), settingsBackupDir, vr,
                                             preferredVrMode, error, L"EliteDangerous64.exe",
                                             lastSettingsSnapshot.get());
    };
    env.restoreLaunchMode = [lastSettingsSnapshot]() {
        restoreSettingsXmlFromSnapshot(*lastSettingsSnapshot);
        *lastSettingsSnapshot = SettingsXmlSnapshot();
    };
    // No askLogin or askCode: a headless start never asks. A Frontier-store direct launch without a stored
    // login stops with a line saying so, and the install's own route is not taken by surprise.
    // No start(): there is no payload to load and no update to check here.
    LauncherSession session(env);

    std::string shown;
    auto show = [&]() {
        if (session.playLine() == shown) return;
        shown = session.playLine();
        if (!shown.empty()) out.line(shown);
    };
    beginHeadlessPlay(session, game, useVr, headlessRouteOps(args.wrappedArgs, useVr), WatchNames(), WatchTiming());
    show();
    bool finished = closeRequested || !session.playing();
    while (!finished) {
        queue.pop()();
        show();
        finished = closeRequested || !session.playing();
    }
    // Elite started (and is either still running, or the wait was ended by --close-after-start): 0. Otherwise 1.
    return session.gameStarted() ? 0 : 1;
}

}  // namespace

int runHeadless(const AppArgs& args, bool haveConsole) {
    Output out(haveConsole);
    const int code = headlessPlay(args, out);
    out.finish();
    return code;
}

void beginHeadlessPlay(LauncherSession& session, const GameInstall& game, bool vr, const RouteOps& ops,
                       const WatchNames& names, const WatchTiming& timing) {
    session.beginPlay(commandSource(game), game.dir, vr, false, ops, names, timing);
}

int directDryRunLines(const AppArgs& args, DirectEnv env, std::vector<std::string>* lines,
                      const std::vector<GameInstall>* installs) {
    if (args.dir.empty() || args.vr == args.novr) {
        lines->push_back("--direct-dry-run needs --dir <the game's product folder> and exactly one of --vr and --novr.");
        return 2;
    }
    GameInstall game;
    const bool described = describeSelectedDir(args, &game, installs);
    if (!described) {
        lines->push_back("No EliteDangerous64.exe under " + narrowUtf8(args.dir) + ". Nothing was started.");
        return 2;
    }
    const std::wstring source = commandSource(game);
    if (_wcsicmp(source.c_str(), L"Epic") == 0) {
        // Nothing is copied and nothing is started. Play would put this launcher in
        // EDLaunch.exe's place; Epic then starts it, and it starts the game itself.
        const LaunchRoute route = routeFor(source, game.dir, args.vr, nullptr, true);
        if (route.kind == RouteKind::None) {
            lines->push_back(route.why.empty() ? "Nothing was started." : route.why + " Nothing was started.");
            return 3;
        }
        lines->push_back(std::string("Dry run: nothing was started. EDLaunch.exe would be this launcher, which starts the game directly in ") +
                         (args.vr ? "VR" : "flat") + " and does not open Frontier's launcher.");
        return 0;
    }
    env.progress = [lines](const std::string& line) { lines->push_back(line); };
    DirectRequest request;
    request.productDir = game.dir;
    request.vr = args.vr;
    request.frontier = _wcsicmp(source.c_str(), L"Frontier launcher") == 0 || args.frontierLogin;
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
    const LauncherRoots roots = launcherRootsFor(args);
    if (roots.canonical) migrateLegacyDataRoot(roots.dataRoot);
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

int runOpenXrProbeCommand(const AppArgs& args) {
    return runOpenXrProbeHelper(args.probeOpenXrLoader, args.probeOpenXrSha);
}

const std::string& launcherStartupNote() { return g_startupNote; }

bool launcherStartup(const AppArgs& args, int* exitCode) {
    *exitCode = 0;
    const LauncherRoots roots = launcherRootsFor(args);
    if (!roots.canonical) return true;
    migrateLegacyDataRoot(roots.dataRoot);

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
