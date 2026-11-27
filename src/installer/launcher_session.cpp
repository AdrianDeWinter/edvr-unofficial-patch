#include "launcher_session.h"

#include <windows.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "frontier_login.h"
#include "game_language.h"
#include "launch_route.h"
#include "launcher_state.h"
#include "probe.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

// The updater's messages are lower-case fragments with no full stop; the window shows sentences.
std::string sentence(std::string text) {
    if (text.empty()) return text;
    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (text.back() != '.') text += '.';
    return text;
}

std::wstring launcherIniPath(const LauncherRoots& roots) { return joinPath(roots.launcherDir(), L"launcher.ini"); }

std::wstring frontierLoginPath(const LauncherRoots& roots) { return joinPath(roots.launcherDir(), kFrontierLoginFile); }

// The installs the launcher can sign in for itself: Steam's, and the one from Frontier's own store.
bool goesDirect(const std::wstring& source) {
    return _wcsicmp(source.c_str(), L"Steam") == 0 || _wcsicmp(source.c_str(), L"Frontier launcher") == 0;
}

// "name 42%" or just the name when the size is not known.
std::string progressText(const std::string& name, unsigned long long received, unsigned long long total) {
    if (total == 0) return "Downloading " + name + "...";
    const unsigned long long percent = received >= total ? 100 : received * 100 / total;
    return "Downloading " + name + "  " + std::to_string(percent) + "%";
}

}  // namespace

std::unique_ptr<HttpClient> LauncherSession::makeHttp() const {
    if (env_.makeHttp) return env_.makeHttp();
    return std::unique_ptr<HttpClient>(new WinHttpClient(false, EDUP_LAUNCHER_VERSION));
}

UpdaterSettings LauncherSession::updaterSettings() const {
    UpdaterSettings s;
    s.launcherDir = env_.roots.launcherDir();
    s.cacheDir = env_.roots.cacheDir();
    s.channel = channel_;
    s.keys = env_.keys;
    if (!env_.apiBase.empty()) s.apiBase = env_.apiBase;
    return s;
}

void LauncherSession::start() {
    LauncherState state;
    if (state.load(launcherIniPath(env_.roots), nullptr)) {
        channel_ = state.channel();
        language_ = state.language();
        theme_ = state.theme();
    }
    if (!env_.channelOverride.empty()) channel_ = env_.channelOverride;

    // The payload first: it is what makes Install available. The window is already up.
    payloadState_ = PayloadState::Loading;
    const unsigned long long gen = ++payloadGen_;
    const LauncherRoots roots = env_.roots;
    const std::wstring payloadDir = env_.payloadDir;
    const char* const* keys = env_.keys;
    spawn([this, roots, payloadDir, keys, gen]() {
        const PayloadLoad load = loadLauncherPayload(roots, payloadDir, keys);
        post([this, load, gen]() { payloadLoaded(load, gen); });
    });
    beginCheck();
}

void LauncherSession::shutdown() {
    cancel_ = true;
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void LauncherSession::payloadLoaded(const PayloadLoad& load, unsigned long long gen, bool notify) {
    if (gen < payloadGen_) return;
    payload_ = load;
    payloadState_ = load.ok ? PayloadState::Ready : PayloadState::None;
    const bool hasEdhmAsset = load.ok && payloadInfo().find("edhm_2dmigoto") != nullptr;
    if (load.ok && load.kind == PayloadKind::Manifest && edhm2dMigoto_ &&
        load.hasOptInEdhmAsset && !hasEdhmAsset) {
        LauncherState state;
        if (state.load(launcherIniPath(env_.roots), nullptr) &&
            !state.currentManifest().empty() &&
            state.edhmOptInFailedManifest() == state.currentManifest()) {
            edhmOptInDownloadFailed_ = true;
            if (edhmWarning_.empty()) edhmWarning_ = kEdhmInterferenceWarning;
        } else {
            edhmOptInDownloadFailed_ = false;
        }
    } else {
        edhmOptInDownloadFailed_ = false;
        // Only clear an opt-in download warning when opt-in is ON and the manifest asset is now present;
        // never clear an antivirus DLL-removal fallback warning set after opting out.
        if (edhm2dMigoto_ && load.kind == PayloadKind::Manifest && hasEdhmAsset &&
            edhmWarning_ == kEdhmInterferenceWarning) {
            edhmWarning_.clear();
        }
    }
    if (notify) changed();
}

void LauncherSession::beginCheck() {
    checkState_ = CheckState::Checking;
    updateLine_ = "Checking for updates...";
    changed();
    UpdaterSettings settings = updaterSettings();
    const std::wstring self = env_.selfExe;
    spawn([this, settings, self]() mutable {
        std::unique_ptr<HttpClient> http = makeHttp();
        if (!self.empty()) settings.launcherSha256 = sha256File(self);
        Updater updater(*http, settings);
        const UpdateCheck result = updater.check();
        post([this, result]() { checkDone(result); });
    });
}

void LauncherSession::checkDone(const UpdateCheck& result) {
    if (updating_) return;  // an update is under way against the check that offered it
    check_ = result;
    switch (result.outcome) {
        case UpdateOutcome::Available:
            checkState_ = CheckState::Available;
            updateLine_ = "ED-UP " + result.manifest.version + " is available.";
            break;
        case UpdateOutcome::UpToDate:
            // Not always "up to date": a release with no signed update, with nothing downloaded yet, lands here too.
            checkState_ = CheckState::UpToDate;
            updateLine_ = sentence(result.message);
            break;
        case UpdateOutcome::Refused:
            checkState_ = CheckState::Refused;
            updateLine_ = sentence(result.message);
            break;
        default:
            checkState_ = CheckState::Skipped;
            updateLine_ = sentence(result.message);
            break;
    }
    changed();
    if (recheckNeeded_ && !updating_) {
        recheckNeeded_ = false;
        if (checkState_ != CheckState::Available) {
            beginCheck();
            return;
        }
    }
    if (autoUpdateAfterCheck_) {
        autoUpdateAfterCheck_ = false;
        if (checkState_ == CheckState::Available && edhm2dMigoto_) {
            beginUpdate();
            return;
        }
    }
    // Started by a self-update: the user already pressed Download, so carry on.
    if (env_.updatedFlag && !autoResumed_ && checkState_ == CheckState::Available) {
        autoResumed_ = true;
        beginUpdate();
    }
}

void LauncherSession::setIncludeReleaseCandidates(bool include) {
    channel_ = include ? "rc" : "stable";
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setChannel(channel_);
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    if (updating_ || checkState_ == CheckState::Checking) return;
    beginCheck();
}

void LauncherSession::updateFailed(const std::string& why) {
    updating_ = false;
    updateLine_ = sentence(why);
    changed();
}

void LauncherSession::beginUpdate() {
    if (checkState_ != CheckState::Available || updating_ || applying_) return;
    const UpdateCheck check = check_;
    const bool canonical = env_.roots.canonical;
    // After a restart for a self-update the launcher is already what the manifest names, or the
    // update would loop: carry on with the payload either way.
    const bool selfUpdate = check.launcherNeedsUpdate && canonical && !env_.updatedFlag;
    updating_ = true;
    updateLine_ = selfUpdate ? "Updating the launcher..." : "Starting the update...";
    changed();

    const UpdaterSettings settings = updaterSettings();
    const LauncherRoots roots = env_.roots;
    const std::wstring self = env_.selfExe;
    const unsigned long long gen = ++payloadGen_;
    spawn([this, check, settings, roots, selfUpdate, self, gen]() mutable {
        std::unique_ptr<HttpClient> http = makeHttp();
        std::string error;
        // Progress is posted when the whole percent changes, not on every chunk.
        auto lastShown = std::make_shared<std::string>();
        DownloadProgress progress = [this, lastShown](const std::string& name, unsigned long long received,
                                                      unsigned long long total) {
            const std::string text = progressText(name, received, total);
            if (text == *lastShown) return;
            *lastShown = text;
            post([this, text]() {
                if (!updating_) return;
                updateLine_ = text;
                changed();
            });
        };
        if (selfUpdate) {
            if (self.empty() || !downloadLauncher(*http, check, self, &cancel_, progress, &error)) {
                post([this, error]() { updateFailed("could not update the launcher: " + error); });
                return;
            }
            post([this]() { finishSelfUpdate(); });
            return;
        }
        Updater updater(*http, settings);
        std::set<std::string> verified;
        std::string optInWarning;
        if (!updater.downloadUpdate(check, &cancel_, progress, &error, &verified, &optInWarning) ||
            !updater.commitManifest(check, &error, &verified)) {
            post([this, error]() { updateFailed("the update did not finish: " + error); });
            return;
        }
        // The payload the window installs from is now the adopted manifest, not a --payload folder.
        const PayloadLoad load = loadLauncherPayload(roots, std::wstring(), settings.keys);
        post([this, load, optInWarning, gen]() {
            updating_ = false;
            const std::wstring prevManifestPath = payload_.manifestPath;
            payloadLoaded(load, gen, false);
            if (!load.ok) {
                updateLine_ = sentence("the update was downloaded but could not be switched to: " + load.note);
                changed();
                return;
            }
            const bool optInMissing =
                edhm2dMigoto_ && load.hasOptInEdhmAsset && payloadInfo().find("edhm_2dmigoto") == nullptr;
            if (!optInWarning.empty()) {
                edhmWarning_ = optInWarning;
            } else if (optInMissing) {
                edhmWarning_ = kEdhmInterferenceWarning;
            } else if (payloadInfo().find("edhm_2dmigoto") != nullptr) {
                edhmWarning_.clear();
            }
            updateLine_ = "ED-UP " + check_.manifest.version + " is ready to install.";
            checkState_ = CheckState::UpToDate;
            changed();
            if (recheckNeeded_) {
                recheckNeeded_ = false;
                if (edhm2dMigoto_ && payload_.ok && payload_.kind == PayloadKind::Manifest &&
                    payload_.hasOptInEdhmAsset && payloadInfo().find("edhm_2dmigoto") == nullptr) {
                    beginCheck();
                }
            }
            // If we only re-adopted the same manifest on a failed opt-in download retry, stay on
            // the current screen so the warning on Settings remains visible.
            const bool sameManifestFailedOptIn =
                !prevManifestPath.empty() && samePath(prevManifestPath, load.manifestPath) &&
                (!optInWarning.empty() || optInMissing);
            if (!sameManifestFailedOptIn && env_.payloadSwitched) env_.payloadSwitched();
        });
    });
}

void LauncherSession::finishSelfUpdate() {
    std::string error;
    const FileMover move = env_.move ? env_.move : FileMover(moveReplacing);
    if (!swapInNewLauncher(env_.selfExe, move, &error)) {
        DeleteFileW(newLauncherPath(env_.selfExe).c_str());
        updateFailed("could not update the launcher: " + error);
        return;
    }
    const bool started = env_.startNewLauncher && env_.startNewLauncher(env_.selfExe);
    if (!started) {
        updating_ = false;
        updateLine_ = "The launcher was updated. Close this window and start ED-UP again to carry on.";
        changed();
        return;
    }
    if (env_.closeWindow) env_.closeWindow();
}

LauncherSession::LauncherSession(SessionEnv env) : env_(std::move(env)) {
    LauncherState state;
    if (state.load(launcherIniPath(env_.roots), nullptr)) {
        directLaunch_ = state.directLaunch();
        closeAfterStart_ = state.closeAfterStart();
        steamFrontierLogin_ = state.steamFrontierLogin();
        vrStereoMode_ = state.vrStereoMode();
        edhm2dMigoto_ = state.edhm2dMigoto();
        edhmOptInDownloadFailed_ =
            edhm2dMigoto_ && !state.currentManifest().empty() &&
            state.edhmOptInFailedManifest() == state.currentManifest();
    }
}

void LauncherSession::setSteamFrontierLogin(bool on, bool clearSavedLogin) {
    if (on == steamFrontierLogin_) return;
    if (!on && clearSavedLogin) deleteFrontierLogin(frontierLoginPath(env_.roots));
    steamFrontierLogin_ = on;
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setSteamFrontierLogin(on);
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    changed();
}

void LauncherSession::setLanguage(const std::string& code) {
    language_ = code;
    LauncherState state;
    if (!state.load(launcherIniPath(env_.roots), nullptr)) return;
    state.setLanguage(code);
    createDirectories(env_.roots.launcherDir());
    state.save(nullptr);
}

void LauncherSession::setTheme(const std::string& mode) {
    theme_ = (mode == "dark" || mode == "light") ? mode : std::string();
    LauncherState state;
    if (!state.load(launcherIniPath(env_.roots), nullptr)) return;
    state.setTheme(theme_);
    createDirectories(env_.roots.launcherDir());
    state.save(nullptr);
}

void LauncherSession::setVrStereoMode(edvr::VrHeadsetMode mode) {
    const edvr::VrHeadsetMode normalized = edvr::vrHeadsetModeFromInt(edvr::vrHeadsetModeToken(mode));
    if (normalized == vrStereoMode_) return;
    vrStereoMode_ = normalized;
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setVrStereoMode(normalized);
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    changed();
}

edvr::VrHeadsetMode LauncherSession::seedVrStereoModeFromSettings(const std::wstring& gameDir) {
    if (vrStereoMode_ != edvr::VrHeadsetMode::NotSet || !env_.detectVrMode) return vrStereoMode_;
    const edvr::VrHeadsetMode detected = env_.detectVrMode(gameDir);
    if (detected != edvr::VrHeadsetMode::NotSet) setVrStereoMode(detected);
    return vrStereoMode_;
}

void LauncherSession::setCloseAfterStart(bool on) {
    if (on == closeAfterStart_) return;
    closeAfterStart_ = on;
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setCloseAfterStart(on);
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    changed();
}

bool LauncherSession::setEdhm2dMigoto(bool on, const std::function<bool()>& confirm) {
    if (on == edhm2dMigoto_) return edhm2dMigoto_;
    if (on && (!confirm || !confirm())) return edhm2dMigoto_;
    edhm2dMigoto_ = on;
    edhmOptInDownloadFailed_ = false;
    if (on) edhmWarning_.clear();
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setEdhm2dMigoto(on);
        state.setEdhmOptInFailedManifest(std::string());
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    if (on && payload_.ok && payload_.kind == PayloadKind::Manifest && payload_.hasOptInEdhmAsset &&
        payloadInfo().find("edhm_2dmigoto") == nullptr) {
        // The adopted schema-3 release carries edhm_2dmigoto, which was skipped while opted out;
        // re-check (or queue one if a check/update is already in flight) so Update can fetch only
        // the opt-in asset on demand.
        if (updating_ || checkState_ == CheckState::Checking) {
            recheckNeeded_ = true;
        } else {
            beginCheck();
        }
    } else if (!on) {
        recheckNeeded_ = false;
        autoUpdateAfterCheck_ = false;
    }
    changed();
    return edhm2dMigoto_;
}

void LauncherSession::retryEdhmOptInDownload() {
    if (!edhm2dMigoto_ || updating_ || applying_) return;
    edhmOptInDownloadFailed_ = false;
    edhmWarning_.clear();
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error) && !state.edhmOptInFailedManifest().empty()) {
        state.setEdhmOptInFailedManifest(std::string());
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    if (checkState_ == CheckState::Available) {
        beginUpdate();
        return;
    }
    autoUpdateAfterCheck_ = true;
    if (checkState_ == CheckState::Checking) {
        recheckNeeded_ = true;
        changed();
    } else {
        beginCheck();
    }
}

void LauncherSession::setEdhmWarning(const std::string& warning) {
    if (edhmWarning_ == warning) return;
    edhmWarning_ = warning;
    changed();
}

void LauncherSession::gameIsRunning() {
    gameStarted_ = true;
    // Close-after-start leaves the game to run on its own, so the window goes; otherwise it steps aside.
    if (closeAfterStart()) {
        if (env_.closeWindow) env_.closeWindow();
    } else if (env_.windowAside) {
        env_.windowAside(true);
    }
}

bool LauncherSession::setDirectLaunch(bool on, const std::function<bool()>& confirm) {
    // Unticking forgets the Frontier login, whether or not the setting was on a moment ago.
    if (!on) deleteFrontierLogin(frontierLoginPath(env_.roots));
    if (on == directLaunch_) return directLaunch_;
    // Only turning it on is a decision to be asked about; a No leaves it exactly as it was.
    if (on && (!confirm || !confirm())) return directLaunch_;
    directLaunch_ = on;
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setDirectLaunch(on);
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    changed();
    return directLaunch_;
}

void LauncherSession::rememberPlay(const std::wstring& gameDir, const char* mode) {
    LauncherState state;
    std::string ignored;
    if (state.load(launcherIniPath(env_.roots), &ignored)) {
        if (mode) state.setMode(gameDir, mode);
        state.setLastInstall(gameDir);
        createDirectories(env_.roots.launcherDir());
        state.save(&ignored);
    }
}

bool LauncherSession::standInForEdLaunch(const std::wstring& gameDir, bool on, bool vr, std::string* error) {
    if (!on) return removeEdLaunchShim(gameDir, error);
    if (!installEdLaunchShim(env_.selfExe, gameDir, error)) return false;
    if (!writeEdLaunchMode(gameDir, vr)) {
        if (error) *error = "The mode for the EDLaunch stand-in could not be written.";
        return false;
    }
    return true;
}

void LauncherSession::beginPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                                const RouteOps& ops, const WatchNames& names, const WatchTiming& timing,
                                const std::function<bool()>& confirmPlay) {
    if (playing_ || applying_) return;
    if (elevated) {
        playLine_ = elevatedRefusal();
        changed();
        return;
    }
    const bool direct = directLaunch() && goesDirect(source);
    const bool epicDirect = directLaunch() && _wcsicmp(source.c_str(), L"Epic") == 0;
    if (!direct) {
        const LaunchRoute route = routeFor(source, gameDir, vr, nullptr, epicDirect);
        if (route.kind == RouteKind::None) {
            playLine_ = route.why.empty() ? "No launch route for this install." : route.why;
            changed();
            return;
        }
    }
    if (confirmPlay && !confirmPlay()) return;
    if (direct) {
        beginDirectPlay(source, gameDir, vr, ops, names, timing);
        return;
    }
    if (epicDirect) {
        std::string error;
        if (!installEdLaunchShim(env_.selfExe, gameDir, &error) || !writeEdLaunchMode(gameDir, vr)) {
            playLine_ = error.empty() ? "The stand-in for EDLaunch.exe could not be written." : error;
            changed();
            return;
        }
    }
    startRouteAndWatch(source, gameDir, vr, elevated, ops, names, timing, "Starting Elite...", epicDirect);
}

void LauncherSession::startRouteAndWatch(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                                         const RouteOps& ops, const WatchNames& names, const WatchTiming& timing,
                                         const std::string& startLine, bool direct, bool allowUnsetVrMode) {
    const LaunchRoute route = routeFor(source, gameDir, vr, nullptr, direct);
    std::string error;
    if (elevated) {
        playLine_ = elevatedRefusal();
        changed();
        return;
    }
    if (route.kind == RouteKind::None) {
        playLine_ = route.why.empty() ? "No launch route for this install." : route.why;
        changed();
        return;
    }
    const bool routeCarriesMode = route.carriesMode || ops.wrappedCarriesMode;
    // If an existing VR player already has mode 3..6 in Settings.xml and vr_3d_mode is still
    // NotSet, seed and persist that choice so it survives between sessions; NotSet stays
    // reserved for a new VR user who has never chosen a headset mode in Settings.xml.
    if (!routeCarriesMode && vr && vrStereoMode_ == edvr::VrHeadsetMode::NotSet) {
        seedVrStereoModeFromSettings(gameDir);
    }
    const bool skipUnsetVr = allowUnsetVrMode && vr && vrStereoMode_ == edvr::VrHeadsetMode::NotSet;
    bool preparedMode = false;
    if (!routeCarriesMode && env_.prepareLaunchMode && !skipUnsetVr) {
        if (!env_.prepareLaunchMode(vr, vrStereoMode_, &error)) {
            playLine_ = error.empty() ? "Elite's Settings.xml could not be updated for this launch." : error;
            changed();
            return;
        }
        preparedMode = true;
    }
    if (!startRoute(route, ops, elevated, &error)) {
        if (preparedMode && env_.restoreLaunchMode) env_.restoreLaunchMode();
        playLine_ = error;
        changed();
        return;
    }
    // Every start remembers its install. Mode is remembered whenever the route carries a mode flag
    // or Settings.xml was updated for the chosen mode.
    const bool modeApplied = routeCarriesMode || (static_cast<bool>(env_.prepareLaunchMode) && !skipUnsetVr);
    rememberPlay(gameDir, modeApplied ? (vr ? "vr" : "flat") : nullptr);
    playing_ = true;
    gameStarted_ = false;
    playLine_ = skipUnsetVr ? (startLine + " Starting in Elite's current 3D mode.") : startLine;
    changed();
    spawn([this, names, timing]() {
        const WatchOutcome outcome = watchLaunch(names, timing, &cancel_, [this](WatchEvent e) {
            post([this, e]() {
                if (e == WatchEvent::WaitingForGame) {
                    playLine_ = "Waiting for Elite to start; if EDLaunch is updating the game, let it finish.";
                } else if (e == WatchEvent::GameSeen) {
                    playLine_ = "Elite is running.";
                    gameIsRunning();
                } else {
                    playLine_ = "Elite has closed.";
                }
                changed();
            });
        });
        post([this, outcome]() {
            playing_ = false;
            switch (outcome.kind) {
                case WatchOutcome::Kind::GameRan:
                    playLine_ = outcome.edlaunchClosed ? "Elite has closed, and so has EDLaunch."
                                                        : "Elite has closed. EDLaunch is still open and was left as it is.";
                    if (env_.windowAside) env_.windowAside(false);
                    break;
                case WatchOutcome::Kind::EdlaunchLeft:
                    playLine_ = "EDLaunch closed before the game started, so there is nothing more to wait for.";
                    break;
                case WatchOutcome::Kind::NeverStarted:
                    playLine_ = "Neither EDLaunch nor Elite started. If you start it from Steam or Epic, check that launcher is running.";
                    break;
                case WatchOutcome::Kind::CouldNotWatch:
                    playLine_ = "Windows would not list the running programs, so the launcher cannot tell when Elite ends.";
                    break;
                default:
                    break;  // cancelled: the window is closing
            }
            changed();
        });
    });
}

// Direct launch (docs\design-launcher-2026-11-07.md, "Direct, on Steam and the Frontier store"): the
// sign-in on a worker, the start, and the wait on the process handle held here. A fallback goes back
// to the window's thread and takes the Steam route, with its reason as the line.
// What a question to the window shares between the worker that asks and the window's thread that answers.
struct LauncherSession::Asked {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    LoginForm form;
    std::string code;
};

std::shared_ptr<LauncherSession::Asked> LauncherSession::askOnWindow(const std::function<bool(Asked&)>& ask) {
    std::shared_ptr<Asked> state = std::make_shared<Asked>();
    post([state, ask]() {
        const bool ok = ask(*state);
        std::lock_guard<std::mutex> lock(state->m);
        state->ok = ok;
        state->done = true;
        state->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(state->m);
    while (!state->done) {
        if (cancel_) return nullptr;  // the window is closing and may never run the job
        state->cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    return state;
}

void LauncherSession::beginDirectPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, const RouteOps& ops,
                                      const WatchNames& names, const WatchTiming& timing) {
    rememberPlay(gameDir, vr ? "vr" : "flat");
    playing_ = true;
    gameStarted_ = false;
    playLine_ = "Signing in to Frontier...";
    changed();
    const std::string base = env_.frontierBase.empty() ? std::string(kFrontierApiBase) : env_.frontierBase;
    const bool frontierStore = _wcsicmp(source.c_str(), L"Steam") != 0 || steamFrontierLogin();
    spawn([this, source, gameDir, vr, ops, names, timing, base, frontierStore]() {
        std::unique_ptr<HttpClient> http = makeHttp();
        DirectEnv direct;
        direct.http = http.get();
        direct.apiBase = base;
        direct.makeSteam = env_.makeSteam;
        direct.machineKeys = env_.machineKeys;
        direct.clock = env_.clock;
        direct.cancel = &cancel_;
        direct.progress = [this](const std::string& line) {
            post([this, line]() {
                if (!playing_) return;
                playLine_ = line;
                changed();
            });
        };
        if (frontierStore) {
            direct.loginPath = frontierLoginPath(env_.roots);
            // The window's modal questions, asked from here: the job runs on the window's thread and
            // this thread waits for the answer.
            if (env_.askLogin && env_.askCode) {
                direct.prompts.askLogin = [this](const std::string& note, LoginForm* form) {
                    const LoginForm known = *form;
                    const std::shared_ptr<Asked> answer = askOnWindow([this, note, known](Asked& a) {
                        a.form = known;
                        return env_.askLogin(note, &a.form);
                    });
                    if (!answer || !answer->ok) return false;
                    *form = answer->form;
                    wipeString(&answer->form.password);
                    return true;
                };
                direct.prompts.askCode = [this](const std::string& email, const std::string& note, std::string* code) {
                    const std::shared_ptr<Asked> answer = askOnWindow([this, email, note](Asked& a) { return env_.askCode(email, note, &a.code); });
                    if (!answer || !answer->ok) return false;
                    *code = answer->code;
                    return true;
                };
            }
        }
        DirectRequest request;
        request.productDir = gameDir;
        request.vr = vr;
        request.frontier = frontierStore;
        request.languageCode = language_.empty() ? systemGameLanguageCode() : canonicalGameLanguage(language_);
        const std::wstring edlaunch = findEdLaunch(gameDir);
        const size_t slash = edlaunch.find_last_of(L"\\/");
        request.languageArg = gameLanguageArgument(slash == std::wstring::npos ? std::wstring() : edlaunch.substr(0, slash), request.languageCode);
        DirectPlan plan = planDirectLaunch(direct, request);
        if (cancel_) return;  // the window is closing

        // Whatever went wrong, the game is not running yet: the install's own route is the way on.
        auto fallBack = [this, source, gameDir, vr, ops, names, timing, frontierStore](const std::string& reason) {
            post([this, source, gameDir, vr, ops, names, timing, reason, frontierStore]() {
                playing_ = false;
                startRouteAndWatch(source, gameDir, vr, false, ops, names, timing,
                                   fallbackLine(reason, frontierStore), false, true);
            });
        };
        if (!plan.start && plan.stopped) {
            // A cancelled question, or a sign-in that cannot go on without one: nothing is started.
            const std::string line = plan.reason;
            post([this, line]() {
                playing_ = false;
                playLine_ = line;
                changed();
            });
            return;
        }
        if (!plan.start) {
            fallBack(plan.reason);
            return;
        }
        const GameStarter start = env_.startGame ? env_.startGame : GameStarter(startGameProcess);
        HANDLE process = nullptr;
        {
            // Only a Steam install's game gets Steam's app id, and only for its own start: the process's
            // environment is put back straight after, so the next start from this window does not inherit it.
            std::unique_ptr<ScopedEnvironmentVariable> appId;
            if (plan.steamAppId) appId.reset(new ScopedEnvironmentVariable(L"SteamAppId", kSteamAppId));
            process = start(plan.exe, plan.args, plan.workDir);
        }
        plan = DirectPlan();  // the tokens are not needed again
        if (!process) {
            fallBack("Elite could not be started directly");
            return;
        }
        post([this]() {
            playLine_ = "Elite is running.";
            gameIsRunning();
            changed();
        });
        while (WaitForSingleObject(process, 200) == WAIT_TIMEOUT) {
            if (cancel_) {
                CloseHandle(process);
                return;  // the window is closing; the game is left running
            }
        }
        CloseHandle(process);
        post([this]() {
            playing_ = false;
            playLine_ = "Elite has closed.";
            if (env_.windowAside) env_.windowAside(false);
            changed();
        });
    });
}

void LauncherSession::beginElevatedApply(const PlanFile& plan, const ElevatedRunner& runner,
                                         std::function<void(const ElevatedResult&)> done) {
    if (applying_) return;
    applying_ = true;
    changed();
    const std::wstring exe = env_.selfExe;
    const std::wstring dir = env_.roots.launcherDir();
    spawn([this, plan, runner, done, exe, dir]() {
        const ElevatedResult result = applyElevated(exe, dir, plan, runner);
        post([this, result, done]() {
            applying_ = false;
            changed();
            if (done) done(result);
        });
    });
}

}  // namespace edvr::installer
